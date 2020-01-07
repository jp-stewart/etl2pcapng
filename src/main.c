/*

Copyright (c) Microsoft Corporation.
Licensed under the MIT License.

etl2pcapng

Converts packet captures in ETL format generated by ndiscap (the ETW provider
in Windows that produces packet capture events) to pcapng format
(readable by Wireshark).

Issues:

-ndiscap supports packet truncation and so does pcapng, but ndiscap doesn't
 currently log metadata about truncation in its events (other than marking
 them with a keyword), so we pretend there is no truncation for now.

*/

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <strsafe.h>
#include <pcapng.h>

#define USAGE \
"etl2pcapng <infile> <outfile>\n" \
"Converts a packet capture from etl to pcapng format.\n"

#define MAX_PACKET_SIZE 65535

// From the ndiscap manifest
#define KW_MEDIA_WIRELESS_WAN         0x200
#define KW_MEDIA_NATIVE_802_11      0x10000
#define KW_PACKET_START          0x40000000
#define KW_PACKET_END            0x80000000
#define KW_SEND                 0x100000000
#define KW_RECEIVE              0x200000000

#define tidPacketFragment            1001
#define tidPacketMetadata            1002
#define tidVMSwitchPacketFragment    1003

// From: https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/windot11/ns-windot11-dot11_extsta_recv_context
#pragma pack(push,8)
typedef struct _NDIS_OBJECT_HEADER {
    UCHAR  Type;
    UCHAR  Revision;
    USHORT Size;
} NDIS_OBJECT_HEADER, * PNDIS_OBJECT_HEADER;

typedef struct DOT11_EXTSTA_RECV_CONTEXT {
    NDIS_OBJECT_HEADER Header;
    ULONG              uReceiveFlags;
    ULONG              uPhyId;
    ULONG              uChCenterFrequency;
    USHORT             usNumberOfMPDUsReceived;
    LONG               lRSSI;
    UCHAR              ucDataRate;
    ULONG              uSizeMediaSpecificInfo;
    PVOID              pvMediaSpecificInfo;
    ULONGLONG          ullTimestamp;
} DOT11_EXTSTA_RECV_CONTEXT, * PDOT11_EXTSTA_RECV_CONTEXT;
#pragma pack(pop)

// From: https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/windot11/ne-windot11-_dot11_phy_type
static const char* DOT11_PHY_TYPE_NAMES[] = {
    "Unknown",        // dot11_phy_type_unknown = 0
    "Fhss",           // dot11_phy_type_fhss = 1
    "Dsss",           // dot11_phy_type_dsss = 2
    "IrBaseband",     // dot11_phy_type_irbaseband = 3
    "802.11a",        // dot11_phy_type_ofdm = 4
    "802.11b",        // dot11_phy_type_hrdsss = 5
    "802.11g",        // dot11_phy_type_erp = 6
    "802.11n",        // dot11_phy_type_ht = 7
    "802.11ac",       // dot11_phy_type_vht = 8
    "802.11ad",       // dot11_phy_type_dmg = 9
    "802.11ax"        // dot11_phy_type_he = 10
};

HANDLE OutFile = INVALID_HANDLE_VALUE;
unsigned long long NumFramesConverted = 0;
BOOLEAN Pass2 = FALSE;
char AuxFragBuf[MAX_PACKET_SIZE] = { 0 };
unsigned long AuxFragBufOffset = 0;

DOT11_EXTSTA_RECV_CONTEXT PacketMetadata;
BOOLEAN AddMetadata = FALSE;

const GUID NdisCapId = { // Microsoft-Windows-NDIS-PacketCapture {2ED6006E-4729-4609-B423-3EE7BCD678EF}
    0x2ed6006e, 0x4729, 0x4609, 0xb4, 0x23, 0x3e, 0xe7, 0xbc, 0xd6, 0x78, 0xef };

struct INTERFACE {
    struct INTERFACE* Next;
    unsigned long LowerIfIndex;
    unsigned long MiniportIfIndex;
    unsigned long PcapNgIfIndex;
    short Type;
};

#define IFACE_HT_SIZE 100
struct INTERFACE* InterfaceHashTable[IFACE_HT_SIZE] = { 0 };
unsigned long NumInterfaces = 0;

struct INTERFACE* GetInterface(unsigned long LowerIfIndex)
{
    struct INTERFACE* Iface = InterfaceHashTable[LowerIfIndex % IFACE_HT_SIZE];
    while (Iface != NULL) {
        if (Iface->LowerIfIndex == LowerIfIndex) {
            return Iface;
        }
        Iface = Iface->Next;
    }
    return NULL;
}

void AddInterface(unsigned long LowerIfIndex, unsigned long MiniportIfIndex, short Type)
{
    struct INTERFACE** Iface = &InterfaceHashTable[LowerIfIndex % IFACE_HT_SIZE];
    struct INTERFACE* NewIface = malloc(sizeof(struct INTERFACE));
    if (NewIface == NULL) {
        printf("out of memory\n");
        exit(1);
    }
    NewIface->LowerIfIndex = LowerIfIndex;
    NewIface->MiniportIfIndex = MiniportIfIndex;
    NewIface->Type = Type;
    NewIface->Next = *Iface;
    *Iface = NewIface;
    NumInterfaces++;
}

int __cdecl InterfaceCompareFn(const void* A, const void* B)
{
    // MiniportIfIndex is the primary sort and LowerIfIndex is
    // the secondary sort, except that inside a group of interfaces
    // with the same MiniportIfIndex we want the one with
    // MiniportIfIndex==LowerIfIndex (i.e. the miniport) to come
    // first.

    unsigned long MA = (*((struct INTERFACE**)A))->MiniportIfIndex;
    unsigned long MB = (*((struct INTERFACE**)B))->MiniportIfIndex;
    unsigned long LA = (*((struct INTERFACE**)A))->LowerIfIndex;
    unsigned long LB = (*((struct INTERFACE**)B))->LowerIfIndex;

    if (MA == MB) {
        if (MA == LA) {
            // A is the miniport.
            return -1;
        } else if (MB == LB) {
            // B is the miniport.
            return 1;
        } else if (LA < LB) {
            return -1;
        } else if (LB < LA) {
            return 1;
        } else {
            return 0;
        }
    } else if (MA < MB) {
        return -1;
    } else {
        return 1;
    }
}

void WriteInterfaces()
{
    // Sorts the interfaces, writes them to the pcapng file, and prints them
    // for user reference.

    struct INTERFACE** InterfaceArray;
    struct INTERFACE* Interface;
    unsigned int i, j;

    InterfaceArray = (struct INTERFACE**)malloc(NumInterfaces * sizeof(struct INTERFACE*));
    if (InterfaceArray == NULL) {
        printf("out of memory\n");
        exit(1);
    }

    j = 0;
    for (i = 0; i < IFACE_HT_SIZE; i++) {
        for (Interface = InterfaceHashTable[i]; Interface != NULL; Interface = Interface->Next) {
            InterfaceArray[j++] = Interface;
        }
    }

    qsort(InterfaceArray, NumInterfaces, sizeof(struct INTERFACE*), InterfaceCompareFn);

    for (i = 0; i < NumInterfaces; i++) {
        Interface = InterfaceArray[i];
        Interface->PcapNgIfIndex = i;
        PcapNgWriteInterfaceDesc(OutFile, Interface->Type, MAX_PACKET_SIZE);

        switch (Interface->Type) {
        case PCAPNG_LINKTYPE_ETHERNET:
            printf("IF: medium=eth  ID=%u\tIfIndex=%u", Interface->PcapNgIfIndex, Interface->LowerIfIndex);
            break;
        case PCAPNG_LINKTYPE_IEEE802_11:
            printf("IF: medium=wifi ID=%u\tIfIndex=%u", Interface->PcapNgIfIndex, Interface->LowerIfIndex);
            break;
        case PCAPNG_LINKTYPE_RAW:
            printf("IF: medium=mbb  ID=%u\tIfIndex=%u", Interface->PcapNgIfIndex, Interface->LowerIfIndex);
            break;
        }
        if (Interface->LowerIfIndex != Interface->MiniportIfIndex) {
            printf("\t(LWF over IfIndex %u)", Interface->MiniportIfIndex);
        }
        printf("\n");
    }

    free(InterfaceArray);
}

inline int
CombineMetadataWithPacket(
    _In_ HANDLE File,
    _In_ PCHAR FragBuf,
    _In_ unsigned long FragLength,
    _In_ long InterfaceId,
    _In_ long IsSend,
    _In_ long TimeStampHigh, // usec (unless if_tsresol is used)
    _In_ long TimeStampLow,
    _In_ PDOT11_EXTSTA_RECV_CONTEXT Metadata,
    _In_ unsigned long ProcessId
)
{

    char Comment[MAX_PACKET_SIZE] = { 0 };
    size_t commentlength = 0;

    HRESULT Err = StringCchPrintfA((char*)&Comment, MAX_PACKET_SIZE, "Packet Metadata: ReceiveFlags:0x%x, PhyType:%s, CenterCh:%u, NumMPDUsReceived:%u, RSSI:%d, DataRate:%u, PID=%d",
        Metadata->uReceiveFlags,
        DOT11_PHY_TYPE_NAMES[Metadata->uPhyId],
        Metadata->uChCenterFrequency,
        Metadata->usNumberOfMPDUsReceived,
        Metadata->lRSSI,
        Metadata->ucDataRate,
        ProcessId);

    if (FAILED(Err))
    {
        printf("Failed converting NdisMetadata to string with error: %u\n", Err);
    }
    else
    {
        Err = StringCchLengthA((char*)&Comment, MAX_PACKET_SIZE, &commentlength);

        if (FAILED(Err))
        {
            printf("Failed getting length of metadata string with error: %u\n", Err);
            commentlength = 0;
            memset(&Comment, 0, MAX_PACKET_SIZE);
        }
    }




    return PcapNgWriteEnhancedPacket(
        File,
        FragBuf,
        sizeof(DOT11_EXTSTA_RECV_CONTEXT) + FragLength,
        InterfaceId,
        IsSend,
        TimeStampHigh,
        TimeStampLow,
        commentlength > 0 ? (char*)&Comment : NULL,
        (USHORT)commentlength);
}

void WINAPI EventCallback(PEVENT_RECORD ev)
{
    int Err;
    unsigned long LowerIfIndex;
    struct INTERFACE* Iface;
    unsigned long FragLength;
    PROPERTY_DATA_DESCRIPTOR Desc;
    ULARGE_INTEGER TimeStamp;

    if (!IsEqualGUID(&ev->EventHeader.ProviderId, &NdisCapId) ||
        (ev->EventHeader.EventDescriptor.Id != tidPacketFragment &&
         ev->EventHeader.EventDescriptor.Id != tidPacketMetadata &&
         ev->EventHeader.EventDescriptor.Id != tidVMSwitchPacketFragment)) {
        return;
    }

    Desc.PropertyName = (ULONGLONG)L"LowerIfIndex";
    Desc.ArrayIndex = ULONG_MAX;
    Err = TdhGetProperty(ev, 0, NULL, 1, &Desc, sizeof(LowerIfIndex), (PBYTE)&LowerIfIndex);
    if (Err != NO_ERROR) {
        printf("TdhGetProperty LowerIfIndex failed with %u\n", Err);
        return;
    }

    Iface = GetInterface(LowerIfIndex);

    if (!Pass2) {
        short Type;
        if (!!(ev->EventHeader.EventDescriptor.Keyword & KW_MEDIA_NATIVE_802_11)) {
            Type = PCAPNG_LINKTYPE_IEEE802_11;
        } else if (!!(ev->EventHeader.EventDescriptor.Keyword & KW_MEDIA_WIRELESS_WAN)) {
            Type = PCAPNG_LINKTYPE_RAW;
        } else {
            Type = PCAPNG_LINKTYPE_ETHERNET;
        }
        // Record the IfIndex if it's a new one.
        if (Iface == NULL) {
            unsigned long MiniportIfIndex;
            Desc.PropertyName = (ULONGLONG)L"MiniportIfIndex";
            Desc.ArrayIndex = ULONG_MAX;
            Err = TdhGetProperty(ev, 0, NULL, 1, &Desc, sizeof(MiniportIfIndex), (PBYTE)&MiniportIfIndex);
            if (Err != NO_ERROR) {
                printf("TdhGetProperty MiniportIfIndex failed with %u\n", Err);
                return;
            }
            AddInterface(LowerIfIndex, MiniportIfIndex, Type);
        } else if (Iface->Type != Type) {
            printf("WARNING: inconsistent media type in packet events!\n");
        }
        return;
    }

    if (Iface == NULL) {
        // We generated the list of interfaces directly from the
        // packet traces themselves, so there must be a bug.
        printf("ERROR: packet with unrecognized IfIndex\n");
        exit(1);
    }

    //Save off Ndis/Wlan metadata to be added to the next packet
    if (ev->EventHeader.EventDescriptor.Id == tidPacketMetadata)
    {
        DWORD MetadataLength = 0;
        Desc.PropertyName = (ULONGLONG)L"MetadataSize";
        Desc.ArrayIndex = ULONG_MAX;
        Err = TdhGetProperty(ev, 0, NULL, 1, &Desc, sizeof(MetadataLength), (PBYTE)&MetadataLength);
        if (Err != NO_ERROR) {
            printf("TdhGetProperty MetadataSize failed with %u\n", Err);
            return;
        }

        if (MetadataLength != sizeof(PacketMetadata))
        {
            printf("Unknown Metadata length. Expected %u, got %u\n", sizeof(DOT11_EXTSTA_RECV_CONTEXT), MetadataLength);
            return;
        }

        Desc.PropertyName = (ULONGLONG)L"Metadata";
        Desc.ArrayIndex = ULONG_MAX;
        Err = TdhGetProperty(ev, 0, NULL, 1, &Desc, MetadataLength, (PBYTE)&PacketMetadata);
        if (Err != NO_ERROR) {
            printf("TdhGetProperty Metadata failed with %u\n", Err);
            return;
        }

        AddMetadata = TRUE;
        return;
    }

    //Save off Ndis/Wlan metadata to be added to the next packet
    if (ev->EventHeader.EventDescriptor.Id == tidPacketMetadata)
    {
        DWORD MetadataLength = 0;
        Desc.PropertyName = (ULONGLONG)L"MetadataSize";
        Desc.ArrayIndex = ULONG_MAX;
        Err = TdhGetProperty(ev, 0, NULL, 1, &Desc, sizeof(MetadataLength), (PBYTE)&MetadataLength);
        if (Err != NO_ERROR) {
            printf("TdhGetProperty MetadataSize failed with %u\n", Err);
            return;
        }

        if (MetadataLength != sizeof(PacketMetadata))
        {
            printf("Unknown Metadata length. Expected %u, got %u\n", sizeof(DOT11_EXTSTA_RECV_CONTEXT), MetadataLength);
            return;
        }

        Desc.PropertyName = (ULONGLONG)L"Metadata";
        Desc.ArrayIndex = ULONG_MAX;
        Err = TdhGetProperty(ev, 0, NULL, 1, &Desc, MetadataLength, (PBYTE)&PacketMetadata);
        if (Err != NO_ERROR) {
            printf("TdhGetProperty Metadata failed with %u\n", Err);
            return;
        }

        AddMetadata = TRUE;
        return;
    }

    // N.B.: Here we are querying the FragmentSize property to get the
    // total size of the packet, and then reading that many bytes from
    // the Fragment property. This is unorthodox (normally you are
    // supposed to use TdhGetPropertySize to get the size of a property)
    // but required due to the way ndiscap puts packet contents in
    // multiple adjacent properties (which happen to be contiguous in
    // memory).

    Desc.PropertyName = (ULONGLONG)L"FragmentSize";
    Desc.ArrayIndex = ULONG_MAX;
    Err = TdhGetProperty(ev, 0, NULL, 1, &Desc, sizeof(FragLength), (PBYTE)&FragLength);
    if (Err != NO_ERROR) {
        printf("TdhGetProperty FragmentSize failed with %u\n", Err);
        return;
    }

    if (FragLength > RTL_NUMBER_OF(AuxFragBuf) - AuxFragBufOffset) {
        printf("Packet too large (size = %u) and skipped\n", AuxFragBufOffset + FragLength);
        return;
    }

    Desc.PropertyName = (ULONGLONG)L"Fragment";
    Desc.ArrayIndex = ULONG_MAX;
    Err = TdhGetProperty(ev, 0, NULL, 1, &Desc, FragLength, (PBYTE)(AuxFragBuf + AuxFragBufOffset));
    if (Err != NO_ERROR) {
        printf("TdhGetProperty Fragment failed with %u\n", Err);
        return;
    }

    // 100ns since 1/1/1601 -> usec since 1/1/1970.
    // The offset of 11644473600 seconds can be calculated with a
    // couple of calls to SystemTimeToFileTime.
    TimeStamp.QuadPart = (ev->EventHeader.TimeStamp.QuadPart / 10) - 11644473600000000ll;

    // The KW_PACKET_START and KW_PACKET_END keywords are used as follows:
    // -A single-event packet has both KW_PACKET_START and KW_PACKET_END.
    // -A multi-event packet consists of an event with KW_PACKET_START followed
    //  by an event with KW_PACKET_END, with zero or more events with neither
    //  keyword in between.
    //
    // So, we accumulate fragments in AuxFragBuf until KW_PACKET_END is
    // encountered, then call PcapNgWriteEnhancedPacket and start over. There's
    // no need for us to even look for KW_PACKET_START.
    //
    // NB: Starting with Windows 8.1, only single-event packets are traced.
    // This logic is here to support packet captures from older systems.

    if (!!(ev->EventHeader.EventDescriptor.Keyword & KW_PACKET_END)) {

        if (ev->EventHeader.EventDescriptor.Keyword & KW_MEDIA_NATIVE_802_11)
        {
            // Clear Protected bit in the case of 802.11
            // Ndis captures will be decrypted in the etl file

            if (AuxFragBuf[1] & 0x40)
            {
                AuxFragBuf[1] = AuxFragBuf[1] & 0xBF; // _1011_1111_ - Clear "Protected Flag"
            }
        }

        if (AddMetadata)
        {
            CombineMetadataWithPacket(
                OutFile,
                AuxFragBuf,
                AuxFragBufOffset + FragLength,
                Iface->PcapNgIfIndex,
                !!(ev->EventHeader.EventDescriptor.Keyword & KW_SEND),
                TimeStamp.HighPart,
                TimeStamp.LowPart,
                &PacketMetadata,
                ev->EventHeader.ProcessId
            );
        }
        else
        {
            // COMMENT_MAX_SIZE must be multiple of 4
            #define COMMENT_MAX_SIZE 16
            char Comment[COMMENT_MAX_SIZE] = { 0 };
            size_t CommentLength = 0;

            if SUCCEEDED(StringCchPrintfA(Comment, COMMENT_MAX_SIZE, "PID=%d", ev->EventHeader.ProcessId)) {
                if FAILED(StringCchLengthA(Comment, COMMENT_MAX_SIZE, &CommentLength)) {
                    CommentLength = 0;
                }
            }

            PcapNgWriteEnhancedPacket(
                OutFile,
                AuxFragBuf,
                AuxFragBufOffset + FragLength,
                Iface->PcapNgIfIndex,
                !!(ev->EventHeader.EventDescriptor.Keyword & KW_SEND),
                TimeStamp.HighPart,
                TimeStamp.LowPart,
                CommentLength > 0 ? (char*)&Comment : NULL,
                (USHORT)CommentLength);
        }

        AddMetadata = FALSE;
        memset(&PacketMetadata, 0, sizeof(DOT11_EXTSTA_RECV_CONTEXT));

        AuxFragBufOffset = 0;
        NumFramesConverted++;
    } else {
        AuxFragBufOffset += FragLength;
    }
}

int __cdecl wmain(int argc, wchar_t** argv)
{
    int Err;
    EVENT_TRACE_LOGFILE LogFile;
    TRACEHANDLE TraceHandle;
    wchar_t* InFileName;
    wchar_t* OutFileName;

    if (argc == 2 &&
        (!wcscmp(argv[1], L"-v") ||
            !wcscmp(argv[1], L"--version"))) {
        printf("etl2pcapng version 1.4.0\n");
        return 0;
    }

    if (argc != 3) {
        printf(USAGE);
        return ERROR_INVALID_PARAMETER;
    }
    InFileName = argv[1];
    OutFileName = argv[2];

    OutFile = CreateFile(OutFileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (OutFile == INVALID_HANDLE_VALUE) {
        Err = GetLastError();
        printf("CreateFile called on %ws failed with %u\n", OutFileName, Err);
        if (Err == ERROR_SHARING_VIOLATION) {
            printf("The file appears to be open already.\n");
        }
        goto Done;
    }

    Err = PcapNgWriteSectionHeader(OutFile);
    if (Err != NO_ERROR) {
        goto Done;
    }

    ZeroMemory(&LogFile, sizeof(LogFile));
    LogFile.LogFileName = InFileName;
    LogFile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
    LogFile.EventRecordCallback = EventCallback;
    LogFile.Context = NULL;

    TraceHandle = OpenTrace(&LogFile);
    if (TraceHandle == INVALID_PROCESSTRACE_HANDLE) {
        Err = GetLastError();
        printf("OpenTrace failed with %u\n", Err);
        goto Done;
    }

    // Read the ETL file twice.
    // Pass1: Gather interface information.
    // Pass2: Convert packet traces.

    Err = ProcessTrace(&TraceHandle, 1, 0, 0);
    if (Err != NO_ERROR) {
        printf("ProcessTrace failed with %u\n", Err);
        goto Done;
    }

    WriteInterfaces();

    Pass2 = TRUE;

    Err = ProcessTrace(&TraceHandle, 1, 0, 0);
    if (Err != NO_ERROR) {
        printf("ProcessTrace failed with %u\n", Err);
        goto Done;
    }

    printf("Converted %llu frames\n", NumFramesConverted);

Done:
    if (OutFile != INVALID_HANDLE_VALUE) {
        CloseHandle(OutFile);
    }
    return Err;
}
