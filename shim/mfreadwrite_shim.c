/*
 * MF Source Reader Shim DLL - v8b (Dynamic Video Decode + Ring Buffers)
 * 
 * Works with a macOS-side decode server (decode_server.sh):
 *   1. Dumps ASF byte stream to C:\movie_N.bin
 *   2. Decode server detects it, runs ffmpeg -> C:\re8_video_N.nv12
 *   3. Shim opens the growing NV12 file, serves real frames via ReadFile
 *   4. Falls back to black NV12 frames if decode not ready
 *
 * Each source reader gets its own video file and parameters.
 * Known videos identified by byte stream length for correct duration reporting.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <stdio.h>
#include <string.h>

typedef UINT32 MediaEventType;
typedef UINT32 MF_SOURCE_READER_FLAG;

/* GUIDs */
DEFINE_GUID(IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);
DEFINE_GUID(IID_IMFSourceReader, 0x70ae66f2, 0xc809, 0x4e4f, 0x89,0x15,0xbd,0xcb,0x40,0x6b,0x79,0x93);
DEFINE_GUID(IID_IMFMediaType, 0x44ae0fa8, 0xea31, 0x4109, 0x8d,0x2e,0x4c,0xae,0x49,0x97,0xc5,0x55);
DEFINE_GUID(IID_IMFAttributes, 0x2cd2d921, 0xc447, 0x44a7, 0xa1,0x3c,0x4a,0xda,0xbf,0xc2,0x47,0xe3);
DEFINE_GUID(IID_IMFSample, 0xc40a00f2, 0xb93a, 0x4d80, 0xae,0x8c,0x5a,0x1c,0x63,0x4f,0x58,0xe4);
DEFINE_GUID(IID_IMFMediaBuffer, 0x045FA593, 0x8799, 0x42b8, 0xBC,0x8D,0x89,0x68,0xC6,0x45,0x35,0x07);
DEFINE_GUID(IID_IMF2DBuffer, 0x7DC9D5F9, 0x9ED9, 0x44ec, 0x9B,0xBF,0x06,0x00,0xBB,0x58,0x9F,0xBB);
DEFINE_GUID(MF_MT_MAJOR_TYPE, 0x48eba18e, 0xf8c9, 0x4687, 0xbf,0x11,0x0a,0x74,0xc9,0xf9,0x6a,0x8f);
DEFINE_GUID(MF_MT_SUBTYPE, 0xf7e34c9a, 0x42e8, 0x4714, 0xb7,0x4b,0xcb,0x29,0xd7,0x2c,0x35,0xe5);
DEFINE_GUID(MFMediaType_Video, 0x73646976, 0x0000, 0x0010, 0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71);
DEFINE_GUID(MFVideoFormat_NV12, 0x3231564E, 0x0000, 0x0010, 0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71);
DEFINE_GUID(MF_MT_FRAME_SIZE, 0x1652c33d, 0xd6b2, 0x4012, 0xb8,0x34,0x72,0x03,0x08,0x49,0xa3,0x7d);
DEFINE_GUID(MF_MT_FRAME_RATE, 0xc459a2e8, 0x3d2c, 0x4e44, 0xb1,0x32,0xfe,0xe5,0x15,0x6c,0x7b,0xb0);
DEFINE_GUID(MF_PD_DURATION, 0x6c990d33, 0xbb8e, 0x477a, 0x85,0x98,0x0d,0x5d,0x96,0xfc,0xd8,0x8a);
DEFINE_GUID(MF_MT_DEFAULT_STRIDE, 0x644b4e48, 0x1e02, 0x4516, 0xb0,0xeb,0xc0,0x1c,0xa9,0xd4,0x9a,0xc6);
DEFINE_GUID(MF_MT_INTERLACE_MODE, 0xe2724bb8, 0xe676, 0x4806, 0xb4,0xb2,0xa8,0xd6,0xef,0xb4,0x4c,0xcd);
DEFINE_GUID(MF_MT_PIXEL_ASPECT_RATIO, 0xc6376a1e, 0x8d0a, 0x4027, 0xbe,0x45,0x6d,0x9a,0x0a,0xd3,0x9b,0xb6);
DEFINE_GUID(MF_MT_SAMPLE_SIZE, 0xDAD3AB78, 0x1990, 0x408b, 0xBC,0xE2,0xEB,0xA6,0x73,0xDA,0xCC,0x10);
DEFINE_GUID(MF_MT_FIXED_SIZE_SAMPLES, 0xb8ebefaf, 0xb718, 0x4e04, 0xb0,0xa9,0x11,0x67,0x75,0xe3,0x32,0x1b);
DEFINE_GUID(MF_MT_ALL_SAMPLES_INDEPENDENT, 0xc9173739, 0x5e56, 0x461c, 0xb7,0x13,0x46,0xfb,0x99,0x5c,0xb9,0x5f);

/* MF error codes */
#define MF_E_INVALIDREQUEST          ((HRESULT)0xC00D36B2)
#define MF_E_INVALIDSTREAMNUMBER     ((HRESULT)0xC00D36B3)
#define MF_E_NO_MORE_TYPES           ((HRESULT)0xC00D36B9)
#define MF_E_END_OF_STREAM           ((HRESULT)0xC00D3E84)

/* MF stream flags */
#define MF_SOURCE_READERF_ENDOFSTREAM           0x00000001
#define MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED 0x00000010
#define MF_SOURCE_READERF_ERROR                 0x00000004

/* MF Source Reader special stream indices */
#define MF_SOURCE_READER_FIRST_VIDEO_STREAM  ((DWORD)0xFFFFFFFC)
#define MF_SOURCE_READER_FIRST_AUDIO_STREAM  ((DWORD)0xFFFFFFFD)
#define MF_SOURCE_READER_ANY_STREAM          ((DWORD)0xFFFFFFFE)
#define MF_SOURCE_READER_ALL_STREAMS         ((DWORD)0xFFFFFFFE)
#define MF_SOURCE_READER_MEDIASOURCE         ((DWORD)0xFFFFFFFF)

/* Video properties - always output 1920x1080 NV12 at 30fps */
#define VIDEO_WIDTH     1920
#define VIDEO_HEIGHT    1080
#define VIDEO_FPS       30
#define FRAME_DURATION  (10000000LL / VIDEO_FPS) /* 333333 = 100ns units per frame */
#define FRAME_SLEEP_MS  33  /* ~30fps real-time pacing */

/* NV12: Y plane = W*H, UV plane = W*H/2, total = W*H*3/2 */
#define NV12_SIZE       (VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2)

typedef LONGLONG MFTIME;

typedef struct _PROPVARIANT {
    USHORT vt;
    USHORT wReserved1;
    USHORT wReserved2;
    USHORT wReserved3;
    union {
        ULONGLONG uhVal;
        LONGLONG hVal;
        ULONG ulVal;
    };
} PROPVARIANT;

/* ============ Known video lookup table ============ */
/* Identified by ASF byte stream length (unique per video) */
typedef struct KnownVideo {
    ULONGLONG asfSize;       /* ASF byte stream length in bytes */
    DWORD     numFrames;     /* Frame count at 30fps output */
    LONGLONG  duration100ns; /* Duration in 100ns units */
    const char *name;        /* Human-readable name for logging */
} KnownVideo;

static const KnownVideo g_knownVideos[] = {
    { 12569765,  150,  50000000LL,     "RE Engine intro" },       /* 5.00s */
    { 6597801,   2134, 711333333LL,    "CAPCOM intro" },          /* 71.13s */
    { 165192945, 4283, 1427666666LL,   "in-game cutscene" },      /* 142.77s */
};
#define NUM_KNOWN_VIDEOS 3
#define DEFAULT_NUM_FRAMES 9000           /* 300s at 30fps for unknown videos */
#define DEFAULT_DURATION (300LL * 10000000LL)  /* 300 seconds in 100ns units */

static const KnownVideo* LookupVideo(ULONGLONG asfSize) {
    for (int i = 0; i < NUM_KNOWN_VIDEOS; i++) {
        if (g_knownVideos[i].asfSize == asfSize) return &g_knownVideos[i];
    }
    return NULL;
}

/* ============ Debug logging ============ */
static FILE *g_logFile = NULL;
static CRITICAL_SECTION g_logLock;
static DWORD g_startTick = 0;

static void LogInit(void) {
    InitializeCriticalSection(&g_logLock);
    g_startTick = GetTickCount();
    g_logFile = fopen("C:\\mf_shim_debug.log", "w");
    if (g_logFile) {
        fprintf(g_logFile, "=== MF Shim DLL v8b (Dynamic Decode + Ring Buffers) Loaded (tick=%lu) ===\n", (unsigned long)g_startTick);
        fflush(g_logFile);
    }
}

static void Log(const char *fmt, ...) {
    if (!g_logFile) return;
    EnterCriticalSection(&g_logLock);
    DWORD elapsed = GetTickCount() - g_startTick;
    DWORD tid = GetCurrentThreadId();
    fprintf(g_logFile, "[%lu.%03lu tid=%lu] ", elapsed/1000, elapsed%1000, (unsigned long)tid);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
    LeaveCriticalSection(&g_logLock);
}

static void LogGUID(const char *prefix, REFGUID guid) {
    Log("%s {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
        prefix, guid->Data1, guid->Data2, guid->Data3,
        guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
        guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
}

/* ============ Stream index resolver ============ */
static int ResolveStreamIndex(DWORD dwStreamIndex, DWORD *pActualIndex) {
    switch (dwStreamIndex) {
        case 0:
        case MF_SOURCE_READER_FIRST_VIDEO_STREAM:
        case MF_SOURCE_READER_ANY_STREAM:
        case MF_SOURCE_READER_MEDIASOURCE:
            *pActualIndex = 0;
            return 0;
        case MF_SOURCE_READER_FIRST_AUDIO_STREAM:
            return -1;
        default:
            if (dwStreamIndex > 0) return -1;
            *pActualIndex = dwStreamIndex;
            return 0;
    }
}

static const char* StreamName(DWORD idx) {
    switch (idx) {
        case 0: return "0";
        case MF_SOURCE_READER_FIRST_VIDEO_STREAM: return "FIRST_VIDEO";
        case MF_SOURCE_READER_FIRST_AUDIO_STREAM: return "FIRST_AUDIO";
        case MF_SOURCE_READER_ANY_STREAM: return "ANY_STREAM";
        case MF_SOURCE_READER_MEDIASOURCE: return "MEDIASOURCE";
        default: return "?";
    }
}

/* ============ Forward declarations ============ */
typedef struct FakeMediaType FakeMediaType;
typedef struct FakeSourceReader FakeSourceReader;
typedef struct FakeMediaBuffer FakeMediaBuffer;
typedef struct FakeSample FakeSample;

/* ============ Static NV12 black frame (fallback) ============ */
static BYTE *g_blackFrame = NULL;

static void InitBlackFrame(void) {
    g_blackFrame = (BYTE*)VirtualAlloc(NULL, NV12_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_blackFrame) {
        memset(g_blackFrame, 0, VIDEO_WIDTH * VIDEO_HEIGHT);
        memset(g_blackFrame + VIDEO_WIDTH * VIDEO_HEIGHT, 0x80, VIDEO_WIDTH * VIDEO_HEIGHT / 2);
        Log("Black NV12 frame initialized: %d bytes", NV12_SIZE);
    }
}

/* ============ FakeMediaBuffer ============ */

typedef struct FakeMediaBufferVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(FakeMediaBuffer *This, REFIID riid, void **ppv);
    ULONG (STDMETHODCALLTYPE *AddRef)(FakeMediaBuffer *This);
    ULONG (STDMETHODCALLTYPE *Release)(FakeMediaBuffer *This);
    HRESULT (STDMETHODCALLTYPE *Lock)(FakeMediaBuffer *This, BYTE **ppbBuffer, DWORD *pcbMaxLength, DWORD *pcbCurrentLength);
    HRESULT (STDMETHODCALLTYPE *Unlock)(FakeMediaBuffer *This);
    HRESULT (STDMETHODCALLTYPE *GetCurrentLength)(FakeMediaBuffer *This, DWORD *pcbCurrentLength);
    HRESULT (STDMETHODCALLTYPE *SetCurrentLength)(FakeMediaBuffer *This, DWORD cbCurrentLength);
    HRESULT (STDMETHODCALLTYPE *GetMaxLength)(FakeMediaBuffer *This, DWORD *pcbMaxLength);
} FakeMediaBufferVtbl;

struct FakeMediaBuffer {
    FakeMediaBufferVtbl *lpVtbl;
    LONG refCount;
    BYTE *data;
    DWORD dataSize;
};

static HRESULT STDMETHODCALLTYPE FMB_QueryInterface(FakeMediaBuffer *This, REFIID riid, void **ppv) {
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IMFMediaBuffer)) {
        *ppv = This;
        InterlockedIncrement(&This->refCount);
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IMF2DBuffer)) {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE FMB_AddRef(FakeMediaBuffer *This) { return InterlockedIncrement(&This->refCount); }
static ULONG STDMETHODCALLTYPE FMB_Release(FakeMediaBuffer *This) {
    LONG r = InterlockedDecrement(&This->refCount);
    if (r <= 0) This->refCount = 1;
    return r > 0 ? r : 1;
}

static HRESULT STDMETHODCALLTYPE FMB_Lock(FakeMediaBuffer *This, BYTE **ppbBuffer, DWORD *pcbMaxLength, DWORD *pcbCurrentLength) {
    if (ppbBuffer) *ppbBuffer = This->data;
    if (pcbMaxLength) *pcbMaxLength = This->dataSize;
    if (pcbCurrentLength) *pcbCurrentLength = This->dataSize;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FMB_Unlock(FakeMediaBuffer *This) { return S_OK; }

static HRESULT STDMETHODCALLTYPE FMB_GetCurrentLength(FakeMediaBuffer *This, DWORD *pcbCurrentLength) {
    if (pcbCurrentLength) *pcbCurrentLength = This->dataSize;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FMB_SetCurrentLength(FakeMediaBuffer *This, DWORD cbCurrentLength) { return S_OK; }

static HRESULT STDMETHODCALLTYPE FMB_GetMaxLength(FakeMediaBuffer *This, DWORD *pcbMaxLength) {
    if (pcbMaxLength) *pcbMaxLength = This->dataSize;
    return S_OK;
}

static FakeMediaBufferVtbl g_fakeMediaBufferVtbl;

/* ============ FakeSample ============ */

typedef struct FakeSampleVtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(FakeSample *This, REFIID riid, void **ppv);
    ULONG (STDMETHODCALLTYPE *AddRef)(FakeSample *This);
    ULONG (STDMETHODCALLTYPE *Release)(FakeSample *This);
    /* IMFAttributes */
    HRESULT (STDMETHODCALLTYPE *GetItem)(FakeSample *This, REFGUID guidKey, PROPVARIANT *pValue);
    HRESULT (STDMETHODCALLTYPE *GetItemType)(FakeSample *This, REFGUID guidKey, void *pType);
    HRESULT (STDMETHODCALLTYPE *CompareItem)(FakeSample *This, REFGUID guidKey, const PROPVARIANT *Value, BOOL *pbResult);
    HRESULT (STDMETHODCALLTYPE *Compare)(FakeSample *This, void *pTheirs, DWORD MatchType, BOOL *pbResult);
    HRESULT (STDMETHODCALLTYPE *GetUINT32)(FakeSample *This, REFGUID guidKey, UINT32 *punValue);
    HRESULT (STDMETHODCALLTYPE *GetUINT64)(FakeSample *This, REFGUID guidKey, UINT64 *punValue);
    HRESULT (STDMETHODCALLTYPE *GetDouble)(FakeSample *This, REFGUID guidKey, double *pfValue);
    HRESULT (STDMETHODCALLTYPE *GetGUID)(FakeSample *This, REFGUID guidKey, GUID *pguidValue);
    HRESULT (STDMETHODCALLTYPE *GetStringLength)(FakeSample *This, REFGUID guidKey, UINT32 *pcchLength);
    HRESULT (STDMETHODCALLTYPE *GetString)(FakeSample *This, REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32 *pcchLength);
    HRESULT (STDMETHODCALLTYPE *GetAllocatedString)(FakeSample *This, REFGUID guidKey, LPWSTR *ppwszValue, UINT32 *pcchLength);
    HRESULT (STDMETHODCALLTYPE *GetBlobSize)(FakeSample *This, REFGUID guidKey, UINT32 *pcbBlobSize);
    HRESULT (STDMETHODCALLTYPE *GetBlob)(FakeSample *This, REFGUID guidKey, UINT8 *pBuf, UINT32 cbBufSize, UINT32 *pcbBlobSize);
    HRESULT (STDMETHODCALLTYPE *GetAllocatedBlob)(FakeSample *This, REFGUID guidKey, UINT8 **ppBuf, UINT32 *pcbSize);
    HRESULT (STDMETHODCALLTYPE *GetUnknown)(FakeSample *This, REFGUID guidKey, REFIID riid, void **ppv);
    HRESULT (STDMETHODCALLTYPE *SetItem)(FakeSample *This, REFGUID guidKey, const PROPVARIANT *Value);
    HRESULT (STDMETHODCALLTYPE *DeleteItem)(FakeSample *This, REFGUID guidKey);
    HRESULT (STDMETHODCALLTYPE *DeleteAllItems)(FakeSample *This);
    HRESULT (STDMETHODCALLTYPE *SetUINT32)(FakeSample *This, REFGUID guidKey, UINT32 unValue);
    HRESULT (STDMETHODCALLTYPE *SetUINT64)(FakeSample *This, REFGUID guidKey, UINT64 unValue);
    HRESULT (STDMETHODCALLTYPE *SetDouble)(FakeSample *This, REFGUID guidKey, double fValue);
    HRESULT (STDMETHODCALLTYPE *SetGUID)(FakeSample *This, REFGUID guidKey, REFGUID guidValue);
    HRESULT (STDMETHODCALLTYPE *SetString)(FakeSample *This, REFGUID guidKey, LPCWSTR wszValue);
    HRESULT (STDMETHODCALLTYPE *SetBlob)(FakeSample *This, REFGUID guidKey, const UINT8 *pBuf, UINT32 cbBufSize);
    HRESULT (STDMETHODCALLTYPE *SetUnknown)(FakeSample *This, REFGUID guidKey, void *pUnknown);
    HRESULT (STDMETHODCALLTYPE *LockStore)(FakeSample *This);
    HRESULT (STDMETHODCALLTYPE *UnlockStore)(FakeSample *This);
    HRESULT (STDMETHODCALLTYPE *GetCount)(FakeSample *This, UINT32 *pcItems);
    HRESULT (STDMETHODCALLTYPE *GetItemByIndex)(FakeSample *This, UINT32 unIndex, GUID *pguidKey, PROPVARIANT *pValue);
    HRESULT (STDMETHODCALLTYPE *CopyAllItems)(FakeSample *This, void *pDest);
    /* IMFSample */
    HRESULT (STDMETHODCALLTYPE *GetSampleFlags)(FakeSample *This, DWORD *pdwSampleFlags);
    HRESULT (STDMETHODCALLTYPE *SetSampleFlags)(FakeSample *This, DWORD dwSampleFlags);
    HRESULT (STDMETHODCALLTYPE *GetSampleTime)(FakeSample *This, LONGLONG *phnsSampleTime);
    HRESULT (STDMETHODCALLTYPE *SetSampleTime)(FakeSample *This, LONGLONG hnsSampleTime);
    HRESULT (STDMETHODCALLTYPE *GetSampleDuration)(FakeSample *This, LONGLONG *phnsSampleDuration);
    HRESULT (STDMETHODCALLTYPE *SetSampleDuration)(FakeSample *This, LONGLONG hnsSampleDuration);
    HRESULT (STDMETHODCALLTYPE *GetBufferCount)(FakeSample *This, DWORD *pdwBufferCount);
    HRESULT (STDMETHODCALLTYPE *GetBufferByIndex)(FakeSample *This, DWORD dwIndex, void **ppBuffer);
    HRESULT (STDMETHODCALLTYPE *ConvertToContiguousBuffer)(FakeSample *This, void **ppBuffer);
    HRESULT (STDMETHODCALLTYPE *AddBuffer)(FakeSample *This, void *pBuffer);
    HRESULT (STDMETHODCALLTYPE *RemoveBufferByIndex)(FakeSample *This, DWORD dwIndex);
    HRESULT (STDMETHODCALLTYPE *RemoveAllBuffers)(FakeSample *This);
    HRESULT (STDMETHODCALLTYPE *GetTotalLength)(FakeSample *This, DWORD *pcbTotalLength);
    HRESULT (STDMETHODCALLTYPE *CopyToBuffer)(FakeSample *This, void *pBuffer);
} FakeSampleVtbl;

struct FakeSample {
    FakeSampleVtbl *lpVtbl;
    LONG refCount;
    LONGLONG sampleTime;
    LONGLONG sampleDuration;
    FakeMediaBuffer *buffer;
};

#define MAX_READERS 8
#define MAX_SAMPLES 8

static FakeSampleVtbl g_fakeSampleVtbl;
static FakeMediaBuffer g_buffers[MAX_SAMPLES];
static FakeSample g_samples[MAX_SAMPLES];
static volatile LONG g_nextSample = 0;

/* ============ FakeSample implementation ============ */

static HRESULT STDMETHODCALLTYPE FS_QueryInterface(FakeSample *This, REFIID riid, void **ppv) {
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IMFSample) || IsEqualGUID(riid, &IID_IMFAttributes)) {
        *ppv = This;
        InterlockedIncrement(&This->refCount);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE FS_AddRef(FakeSample *This) { return InterlockedIncrement(&This->refCount); }
static ULONG STDMETHODCALLTYPE FS_Release(FakeSample *This) {
    LONG r = InterlockedDecrement(&This->refCount);
    if (r <= 0) This->refCount = 1;
    return r > 0 ? r : 1;
}

static HRESULT STDMETHODCALLTYPE FS_Stub(void) { return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE FS_GetSampleFlags(FakeSample *This, DWORD *pdwSampleFlags) {
    if (pdwSampleFlags) *pdwSampleFlags = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FS_SetSampleFlags(FakeSample *This, DWORD dwSampleFlags) { return S_OK; }

static HRESULT STDMETHODCALLTYPE FS_GetSampleTime(FakeSample *This, LONGLONG *phnsSampleTime) {
    if (phnsSampleTime) *phnsSampleTime = This->sampleTime;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FS_SetSampleTime(FakeSample *This, LONGLONG hnsSampleTime) {
    This->sampleTime = hnsSampleTime;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FS_GetSampleDuration(FakeSample *This, LONGLONG *phnsSampleDuration) {
    if (phnsSampleDuration) *phnsSampleDuration = This->sampleDuration;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FS_SetSampleDuration(FakeSample *This, LONGLONG hnsSampleDuration) {
    This->sampleDuration = hnsSampleDuration;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FS_GetBufferCount(FakeSample *This, DWORD *pdwBufferCount) {
    if (pdwBufferCount) *pdwBufferCount = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FS_GetBufferByIndex(FakeSample *This, DWORD dwIndex, void **ppBuffer) {
    if (dwIndex != 0 || !ppBuffer) return E_INVALIDARG;
    *ppBuffer = This->buffer;
    InterlockedIncrement(&This->buffer->refCount);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FS_ConvertToContiguousBuffer(FakeSample *This, void **ppBuffer) {
    if (!ppBuffer) return E_INVALIDARG;
    *ppBuffer = This->buffer;
    InterlockedIncrement(&This->buffer->refCount);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FS_AddBuffer(FakeSample *This, void *pBuffer) { return S_OK; }
static HRESULT STDMETHODCALLTYPE FS_RemoveBufferByIndex(FakeSample *This, DWORD dwIndex) { return S_OK; }
static HRESULT STDMETHODCALLTYPE FS_RemoveAllBuffers(FakeSample *This) { return S_OK; }

static HRESULT STDMETHODCALLTYPE FS_GetTotalLength(FakeSample *This, DWORD *pcbTotalLength) {
    if (pcbTotalLength) *pcbTotalLength = NV12_SIZE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FS_CopyToBuffer(FakeSample *This, void *pBuffer) { return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE FS_GetUINT32(FakeSample *This, REFGUID guidKey, UINT32 *punValue) {
    if (punValue) *punValue = 0;
    return MF_E_INVALIDREQUEST;
}
static HRESULT STDMETHODCALLTYPE FS_GetUINT64(FakeSample *This, REFGUID guidKey, UINT64 *punValue) {
    if (punValue) *punValue = 0;
    return MF_E_INVALIDREQUEST;
}
static HRESULT STDMETHODCALLTYPE FS_GetCount(FakeSample *This, UINT32 *pcItems) {
    if (pcItems) *pcItems = 0;
    return S_OK;
}

/* Get a sample from pool, pointing to given data */
static FakeSample* GetFakeSample(LONGLONG timestamp, LONGLONG duration, BYTE *frameData) {
    LONG idx = InterlockedIncrement(&g_nextSample) - 1;
    idx = idx % MAX_SAMPLES;
    FakeSample *s = &g_samples[idx];
    s->refCount = 1;
    s->sampleTime = timestamp;
    s->sampleDuration = duration;
    s->buffer = &g_buffers[idx];
    s->buffer->refCount = 1;
    s->buffer->data = frameData;
    s->buffer->dataSize = NV12_SIZE;
    return s;
}

/* ============ FakeMediaType ============ */
typedef struct FakeMediaTypeVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(FakeMediaType *This, REFIID riid, void **ppv);
    ULONG (STDMETHODCALLTYPE *AddRef)(FakeMediaType *This);
    ULONG (STDMETHODCALLTYPE *Release)(FakeMediaType *This);
    HRESULT (STDMETHODCALLTYPE *GetItem)(FakeMediaType *This, REFGUID guidKey, PROPVARIANT *pValue);
    HRESULT (STDMETHODCALLTYPE *GetItemType)(FakeMediaType *This, REFGUID guidKey, void *pType);
    HRESULT (STDMETHODCALLTYPE *CompareItem)(FakeMediaType *This, REFGUID guidKey, const PROPVARIANT *Value, BOOL *pbResult);
    HRESULT (STDMETHODCALLTYPE *Compare)(FakeMediaType *This, void *pTheirs, DWORD MatchType, BOOL *pbResult);
    HRESULT (STDMETHODCALLTYPE *GetUINT32)(FakeMediaType *This, REFGUID guidKey, UINT32 *punValue);
    HRESULT (STDMETHODCALLTYPE *GetUINT64)(FakeMediaType *This, REFGUID guidKey, UINT64 *punValue);
    HRESULT (STDMETHODCALLTYPE *GetDouble)(FakeMediaType *This, REFGUID guidKey, double *pfValue);
    HRESULT (STDMETHODCALLTYPE *GetGUID)(FakeMediaType *This, REFGUID guidKey, GUID *pguidValue);
    HRESULT (STDMETHODCALLTYPE *GetStringLength)(FakeMediaType *This, REFGUID guidKey, UINT32 *pcchLength);
    HRESULT (STDMETHODCALLTYPE *GetString)(FakeMediaType *This, REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32 *pcchLength);
    HRESULT (STDMETHODCALLTYPE *GetAllocatedString)(FakeMediaType *This, REFGUID guidKey, LPWSTR *ppwszValue, UINT32 *pcchLength);
    HRESULT (STDMETHODCALLTYPE *GetBlobSize)(FakeMediaType *This, REFGUID guidKey, UINT32 *pcbBlobSize);
    HRESULT (STDMETHODCALLTYPE *GetBlob)(FakeMediaType *This, REFGUID guidKey, UINT8 *pBuf, UINT32 cbBufSize, UINT32 *pcbBlobSize);
    HRESULT (STDMETHODCALLTYPE *GetAllocatedBlob)(FakeMediaType *This, REFGUID guidKey, UINT8 **ppBuf, UINT32 *pcbSize);
    HRESULT (STDMETHODCALLTYPE *GetUnknown)(FakeMediaType *This, REFGUID guidKey, REFIID riid, void **ppv);
    HRESULT (STDMETHODCALLTYPE *SetItem)(FakeMediaType *This, REFGUID guidKey, const PROPVARIANT *Value);
    HRESULT (STDMETHODCALLTYPE *DeleteItem)(FakeMediaType *This, REFGUID guidKey);
    HRESULT (STDMETHODCALLTYPE *DeleteAllItems)(FakeMediaType *This);
    HRESULT (STDMETHODCALLTYPE *SetUINT32)(FakeMediaType *This, REFGUID guidKey, UINT32 unValue);
    HRESULT (STDMETHODCALLTYPE *SetUINT64)(FakeMediaType *This, REFGUID guidKey, UINT64 unValue);
    HRESULT (STDMETHODCALLTYPE *SetDouble)(FakeMediaType *This, REFGUID guidKey, double fValue);
    HRESULT (STDMETHODCALLTYPE *SetGUID)(FakeMediaType *This, REFGUID guidKey, REFGUID guidValue);
    HRESULT (STDMETHODCALLTYPE *SetString)(FakeMediaType *This, REFGUID guidKey, LPCWSTR wszValue);
    HRESULT (STDMETHODCALLTYPE *SetBlob)(FakeMediaType *This, REFGUID guidKey, const UINT8 *pBuf, UINT32 cbBufSize);
    HRESULT (STDMETHODCALLTYPE *SetUnknown)(FakeMediaType *This, REFGUID guidKey, void *pUnknown);
    HRESULT (STDMETHODCALLTYPE *LockStore)(FakeMediaType *This);
    HRESULT (STDMETHODCALLTYPE *UnlockStore)(FakeMediaType *This);
    HRESULT (STDMETHODCALLTYPE *GetCount)(FakeMediaType *This, UINT32 *pcItems);
    HRESULT (STDMETHODCALLTYPE *GetItemByIndex)(FakeMediaType *This, UINT32 unIndex, GUID *pguidKey, PROPVARIANT *pValue);
    HRESULT (STDMETHODCALLTYPE *CopyAllItems)(FakeMediaType *This, void *pDest);
    HRESULT (STDMETHODCALLTYPE *GetMajorType)(FakeMediaType *This, GUID *pguidMajorType);
    HRESULT (STDMETHODCALLTYPE *IsCompressedFormat)(FakeMediaType *This, BOOL *pfCompressed);
    HRESULT (STDMETHODCALLTYPE *IsEqual)(FakeMediaType *This, void *pIMediaType, DWORD *pdwFlags);
    HRESULT (STDMETHODCALLTYPE *GetRepresentation)(FakeMediaType *This, GUID guidRepresentation, void **ppvRepresentation);
    HRESULT (STDMETHODCALLTYPE *FreeRepresentation)(FakeMediaType *This, GUID guidRepresentation, void *pvRepresentation);
} FakeMediaTypeVtbl;

struct FakeMediaType {
    FakeMediaTypeVtbl *lpVtbl;
    LONG refCount;
};

static HRESULT STDMETHODCALLTYPE FMT_QueryInterface(FakeMediaType *This, REFIID riid, void **ppv) {
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IMFMediaType) || IsEqualGUID(riid, &IID_IMFAttributes)) {
        *ppv = This;
        InterlockedIncrement(&This->refCount);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE FMT_AddRef(FakeMediaType *This) { return InterlockedIncrement(&This->refCount); }
static ULONG STDMETHODCALLTYPE FMT_Release(FakeMediaType *This) {
    LONG r = InterlockedDecrement(&This->refCount);
    if (r <= 0) This->refCount = 1;
    return r > 0 ? r : 1;
}

static HRESULT STDMETHODCALLTYPE FMT_GetItem(FakeMediaType *This, REFGUID guidKey, PROPVARIANT *pValue) {
    if (IsEqualGUID(guidKey, &MF_MT_MAJOR_TYPE)) { if (pValue) pValue->vt = 72; return S_OK; }
    if (IsEqualGUID(guidKey, &MF_MT_SUBTYPE)) { return S_OK; }
    if (IsEqualGUID(guidKey, &MF_MT_FRAME_SIZE)) { return S_OK; }
    if (IsEqualGUID(guidKey, &MF_MT_FRAME_RATE)) { return S_OK; }
    return MF_E_INVALIDREQUEST;
}

static HRESULT STDMETHODCALLTYPE FMT_GetUINT32(FakeMediaType *This, REFGUID guidKey, UINT32 *punValue) {
    if (IsEqualGUID(guidKey, &MF_MT_INTERLACE_MODE)) { *punValue = 2; return S_OK; }
    if (IsEqualGUID(guidKey, &MF_MT_DEFAULT_STRIDE)) { *punValue = VIDEO_WIDTH; return S_OK; }
    if (IsEqualGUID(guidKey, &MF_MT_FIXED_SIZE_SAMPLES)) { *punValue = 1; return S_OK; }
    if (IsEqualGUID(guidKey, &MF_MT_ALL_SAMPLES_INDEPENDENT)) { *punValue = 1; return S_OK; }
    if (IsEqualGUID(guidKey, &MF_MT_SAMPLE_SIZE)) { *punValue = NV12_SIZE; return S_OK; }
    *punValue = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FMT_GetUINT64(FakeMediaType *This, REFGUID guidKey, UINT64 *punValue) {
    if (IsEqualGUID(guidKey, &MF_MT_FRAME_SIZE)) {
        *punValue = ((UINT64)VIDEO_WIDTH << 32) | VIDEO_HEIGHT;
        return S_OK;
    }
    if (IsEqualGUID(guidKey, &MF_MT_FRAME_RATE)) {
        *punValue = ((UINT64)VIDEO_FPS << 32) | 1;
        return S_OK;
    }
    if (IsEqualGUID(guidKey, &MF_MT_PIXEL_ASPECT_RATIO)) {
        *punValue = ((UINT64)1 << 32) | 1;
        return S_OK;
    }
    *punValue = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FMT_GetGUID(FakeMediaType *This, REFGUID guidKey, GUID *pguidValue) {
    if (IsEqualGUID(guidKey, &MF_MT_MAJOR_TYPE)) { *pguidValue = MFMediaType_Video; return S_OK; }
    if (IsEqualGUID(guidKey, &MF_MT_SUBTYPE)) { *pguidValue = MFVideoFormat_NV12; return S_OK; }
    memset(pguidValue, 0, sizeof(GUID));
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE FMT_GetMajorType(FakeMediaType *This, GUID *pguidMajorType) {
    *pguidMajorType = MFMediaType_Video;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FMT_IsCompressedFormat(FakeMediaType *This, BOOL *pfCompressed) {
    *pfCompressed = FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FMT_Stub(void) { return E_NOTIMPL; }

/* ============ FakeSourceReader ============ */

/* Per-reader video state */
typedef struct ReaderVideo {
    HANDLE nv12File;        /* Handle to re8_video_N.nv12 */
    BYTE *frameBufs[MAX_SAMPLES]; /* Ring of frame buffers (one per sample slot) */
    DWORD numFrames;        /* Total expected frames */
    LONGLONG duration100ns; /* Duration in 100ns units */
    LONG readerNum;         /* Reader number for file naming */
    ULONGLONG asfSize;      /* ASF byte stream size (for identification) */
} ReaderVideo;

typedef struct FakeSourceReaderVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(FakeSourceReader *This, REFIID riid, void **ppv);
    ULONG (STDMETHODCALLTYPE *AddRef)(FakeSourceReader *This);
    ULONG (STDMETHODCALLTYPE *Release)(FakeSourceReader *This);
    HRESULT (STDMETHODCALLTYPE *GetStreamSelection)(FakeSourceReader *This, DWORD dwStreamIndex, BOOL *pfSelected);
    HRESULT (STDMETHODCALLTYPE *SetStreamSelection)(FakeSourceReader *This, DWORD dwStreamIndex, BOOL fSelected);
    HRESULT (STDMETHODCALLTYPE *GetNativeMediaType)(FakeSourceReader *This, DWORD dwStreamIndex, DWORD dwMediaTypeIndex, void **ppMediaType);
    HRESULT (STDMETHODCALLTYPE *GetCurrentMediaType)(FakeSourceReader *This, DWORD dwStreamIndex, void **ppMediaType);
    HRESULT (STDMETHODCALLTYPE *SetCurrentMediaType)(FakeSourceReader *This, DWORD dwStreamIndex, DWORD *pdwReserved, void *pMediaType);
    HRESULT (STDMETHODCALLTYPE *SetCurrentPosition)(FakeSourceReader *This, REFGUID guidTimeFormat, const PROPVARIANT *varPosition);
    HRESULT (STDMETHODCALLTYPE *ReadSample)(FakeSourceReader *This, DWORD dwStreamIndex, DWORD dwControlFlags, DWORD *pdwActualStreamIndex, DWORD *pdwStreamFlags, LONGLONG *pllTimestamp, void **ppSample);
    HRESULT (STDMETHODCALLTYPE *Flush)(FakeSourceReader *This, DWORD dwStreamIndex);
    HRESULT (STDMETHODCALLTYPE *GetServiceForStream)(FakeSourceReader *This, DWORD dwStreamIndex, REFGUID guidService, REFIID riid, void **ppvObject);
    HRESULT (STDMETHODCALLTYPE *GetPresentationAttribute)(FakeSourceReader *This, DWORD dwStreamIndex, REFGUID guidAttribute, PROPVARIANT *pvarAttribute);
} FakeSourceReaderVtbl;

struct FakeSourceReader {
    FakeSourceReaderVtbl *lpVtbl;
    LONG refCount;
    FakeMediaType *mediaType;
    LONG readCount;
    LONG id;
    ReaderVideo video;
};

static FakeMediaTypeVtbl g_fakeMediaTypeVtbl;
static FakeSourceReaderVtbl g_fakeSourceReaderVtbl;
static FakeMediaType g_mediaTypes[MAX_READERS];
static FakeSourceReader g_readers[MAX_READERS];
static volatile LONG g_nextReader = 0;

/* ============ Per-reader video file operations ============ */

/* Try to open the NV12 file for this reader */
static void TryOpenNV12(FakeSourceReader *reader) {
    if (reader->video.nv12File != INVALID_HANDLE_VALUE) return; /* Already open */

    char path[128];
    sprintf(path, "C:\\re8_video_%ld.nv12", reader->video.readerNum);

    reader->video.nv12File = CreateFileA(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, /* Allow decode server to still write */
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (reader->video.nv12File != INVALID_HANDLE_VALUE) {
        Log("R%ld: Opened %s", reader->id, path);
    }
}

/* Read frame N from the NV12 file into destBuf. Returns TRUE if successful. */
static BOOL ReadFrameFromFile(FakeSourceReader *reader, DWORD frameIndex, BYTE *destBuf) {
    if (reader->video.nv12File == INVALID_HANDLE_VALUE) return FALSE;

    ULONGLONG offset = (ULONGLONG)frameIndex * NV12_SIZE;
    LONG offsetHigh = (LONG)(offset >> 32);
    DWORD offsetLow = (DWORD)(offset & 0xFFFFFFFF);

    SetFilePointer(reader->video.nv12File, offsetLow, &offsetHigh, FILE_BEGIN);

    DWORD totalRead = 0;
    while (totalRead < NV12_SIZE) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(reader->video.nv12File, destBuf + totalRead,
                           NV12_SIZE - totalRead, &bytesRead, NULL);
        if (!ok || bytesRead == 0) return FALSE;
        totalRead += bytesRead;
    }
    return TRUE;
}

/* Wait for frame N to be available in the NV12 file (file may still be growing) */
static BOOL WaitForFrame(FakeSourceReader *reader, DWORD frameIndex, int maxWaitMs) {
    ULONGLONG neededSize = (ULONGLONG)(frameIndex + 1) * NV12_SIZE;
    int waited = 0;

    while (waited < maxWaitMs) {
        /* Try to open file if not yet open */
        TryOpenNV12(reader);
        if (reader->video.nv12File == INVALID_HANDLE_VALUE) {
            Sleep(100);
            waited += 100;
            continue;
        }

        /* Check current file size */
        DWORD sizeHigh = 0;
        DWORD sizeLow = GetFileSize(reader->video.nv12File, &sizeHigh);
        ULONGLONG fileSize = ((ULONGLONG)sizeHigh << 32) | sizeLow;

        if (fileSize >= neededSize) return TRUE;

        Sleep(50);
        waited += 50;
    }
    return FALSE;
}

/* ============ FakeSourceReader implementation ============ */

static HRESULT STDMETHODCALLTYPE FSR_QueryInterface(FakeSourceReader *This, REFIID riid, void **ppv) {
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IMFSourceReader)) {
        *ppv = This;
        InterlockedIncrement(&This->refCount);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE FSR_AddRef(FakeSourceReader *This) { return InterlockedIncrement(&This->refCount); }
static ULONG STDMETHODCALLTYPE FSR_Release(FakeSourceReader *This) {
    LONG r = InterlockedDecrement(&This->refCount);
    Log("R%ld: Release -> %ld", This->id, r);
    if (r <= 0) {
        /* Clean up per-reader video state */
        if (This->video.nv12File != INVALID_HANDLE_VALUE) {
            CloseHandle(This->video.nv12File);
            This->video.nv12File = INVALID_HANDLE_VALUE;
            Log("R%ld: Closed NV12 file", This->id);
        }
        for (int i = 0; i < MAX_SAMPLES; i++) {
            if (This->video.frameBufs[i]) {
                VirtualFree(This->video.frameBufs[i], 0, MEM_RELEASE);
                This->video.frameBufs[i] = NULL;
            }
        }
        This->refCount = 1;
    }
    return r > 0 ? r : 1;
}

static HRESULT STDMETHODCALLTYPE FSR_GetStreamSelection(FakeSourceReader *This, DWORD dwStreamIndex, BOOL *pfSelected) {
    DWORD actual;
    if (ResolveStreamIndex(dwStreamIndex, &actual) < 0) {
        if (pfSelected) *pfSelected = FALSE;
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (pfSelected) *pfSelected = TRUE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FSR_SetStreamSelection(FakeSourceReader *This, DWORD dwStreamIndex, BOOL fSelected) {
    if (dwStreamIndex == MF_SOURCE_READER_ALL_STREAMS) return S_OK;
    DWORD actual;
    if (ResolveStreamIndex(dwStreamIndex, &actual) < 0) return MF_E_INVALIDSTREAMNUMBER;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FSR_GetNativeMediaType(FakeSourceReader *This, DWORD dwStreamIndex, DWORD dwMediaTypeIndex, void **ppMediaType) {
    DWORD actual;
    if (ResolveStreamIndex(dwStreamIndex, &actual) < 0) {
        *ppMediaType = NULL;
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (dwMediaTypeIndex > 0) {
        *ppMediaType = NULL;
        return MF_E_NO_MORE_TYPES;
    }
    *ppMediaType = This->mediaType;
    InterlockedIncrement(&This->mediaType->refCount);
    Log("R%ld: GetNativeMediaType(%s, %lu) -> S_OK", This->id, StreamName(dwStreamIndex), dwMediaTypeIndex);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FSR_GetCurrentMediaType(FakeSourceReader *This, DWORD dwStreamIndex, void **ppMediaType) {
    DWORD actual;
    if (ResolveStreamIndex(dwStreamIndex, &actual) < 0) {
        if (ppMediaType) *ppMediaType = NULL;
        return MF_E_INVALIDSTREAMNUMBER;
    }
    *ppMediaType = This->mediaType;
    InterlockedIncrement(&This->mediaType->refCount);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FSR_SetCurrentMediaType(FakeSourceReader *This, DWORD dwStreamIndex, DWORD *pdwReserved, void *pMediaType) {
    DWORD actual;
    if (ResolveStreamIndex(dwStreamIndex, &actual) < 0) return MF_E_INVALIDSTREAMNUMBER;
    Log("R%ld: SetCurrentMediaType(%s) -> S_OK", This->id, StreamName(dwStreamIndex));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FSR_SetCurrentPosition(FakeSourceReader *This, REFGUID guidTimeFormat, const PROPVARIANT *varPosition) {
    Log("R%ld: SetCurrentPosition (reset readCount)", This->id);
    InterlockedExchange(&This->readCount, 0);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FSR_ReadSample(FakeSourceReader *This, DWORD dwStreamIndex, DWORD dwControlFlags,
                                                  DWORD *pdwActualStreamIndex, DWORD *pdwStreamFlags,
                                                  LONGLONG *pllTimestamp, void **ppSample) {
    LONG count = InterlockedIncrement(&This->readCount);
    DWORD actual = 0;
    ResolveStreamIndex(dwStreamIndex, &actual);

    if (pdwActualStreamIndex) *pdwActualStreamIndex = actual;
    if (pllTimestamp) *pllTimestamp = 0;
    if (ppSample) *ppSample = NULL;
    if (pdwStreamFlags) *pdwStreamFlags = 0;

    DWORD numFrames = This->video.numFrames;

    if (count <= (LONG)numFrames) {
        /* Pace frames at real-time speed */
        if (count > 1) {
            Sleep(FRAME_SLEEP_MS);
        }

        LONG frameIndex = count - 1;
        LONG sampleIdx = frameIndex % MAX_SAMPLES;
        BYTE *destBuf = This->video.frameBufs[sampleIdx];
        BYTE *frameData = g_blackFrame; /* Default to black */
        const char *source = "BLACK";

        /* On first frame, wait longer for NV12 file to appear (decode server starting) */
        int waitMs = (count == 1) ? 30000 : 5000;

        /* Try to read real frame from NV12 file into this sample's own buffer */
        if (destBuf && WaitForFrame(This, (DWORD)frameIndex, waitMs)) {
            if (ReadFrameFromFile(This, (DWORD)frameIndex, destBuf)) {
                frameData = destBuf;
                source = "REAL";
            }
        }

        LONGLONG timestamp = (LONGLONG)frameIndex * FRAME_DURATION;
        FakeSample *sample = GetFakeSample(timestamp, FRAME_DURATION, frameData);
        if (ppSample) *ppSample = sample;
        if (pllTimestamp) *pllTimestamp = timestamp;

        /* Log at key frame numbers */
        if (count <= 3 || count == 10 || count == 50 || count == 100 || count == 500 ||
            count == 1000 || count == 2000 || count == (LONG)numFrames) {
            Log("R%ld: ReadSample #%ld/%lu (%s) -> ts=%lld [%s]",
                This->id, count, numFrames, StreamName(dwStreamIndex), timestamp, source);
        }
        return S_OK;
    } else {
        /* EOS */
        if (pdwStreamFlags) *pdwStreamFlags = MF_SOURCE_READERF_ENDOFSTREAM;
        if (count <= (LONG)numFrames + 3) {
            Log("R%ld: ReadSample #%ld (%s) -> EOS", This->id, count, StreamName(dwStreamIndex));
        }
        return S_OK;
    }
}

static HRESULT STDMETHODCALLTYPE FSR_Flush(FakeSourceReader *This, DWORD dwStreamIndex) {
    Log("R%ld: Flush(%s)", This->id, StreamName(dwStreamIndex));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FSR_GetServiceForStream(FakeSourceReader *This, DWORD dwStreamIndex, REFGUID guidService, REFIID riid, void **ppvObject) {
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE FSR_GetPresentationAttribute(FakeSourceReader *This, DWORD dwStreamIndex, REFGUID guidAttribute, PROPVARIANT *pvarAttribute) {
    if (IsEqualGUID(guidAttribute, &MF_PD_DURATION)) {
        pvarAttribute->vt = 20; /* VT_UI8 */
        pvarAttribute->uhVal = (ULONGLONG)This->video.duration100ns;
        Log("R%ld: GetPresentationAttribute(DURATION) -> %lldms (%lu frames)",
            This->id, This->video.duration100ns / 10000, This->video.numFrames);
        return S_OK;
    }
    return E_NOTIMPL;
}

/* ============ Init ============ */

static void InitVtables(void) {
    /* MediaBuffer vtable */
    g_fakeMediaBufferVtbl.QueryInterface = FMB_QueryInterface;
    g_fakeMediaBufferVtbl.AddRef = FMB_AddRef;
    g_fakeMediaBufferVtbl.Release = FMB_Release;
    g_fakeMediaBufferVtbl.Lock = FMB_Lock;
    g_fakeMediaBufferVtbl.Unlock = FMB_Unlock;
    g_fakeMediaBufferVtbl.GetCurrentLength = FMB_GetCurrentLength;
    g_fakeMediaBufferVtbl.SetCurrentLength = FMB_SetCurrentLength;
    g_fakeMediaBufferVtbl.GetMaxLength = FMB_GetMaxLength;

    /* Sample vtable */
    void **svptr = (void**)&g_fakeSampleVtbl;
    for (int i = 0; i < (int)(sizeof(FakeSampleVtbl)/sizeof(void*)); i++)
        svptr[i] = (void*)FS_Stub;
    g_fakeSampleVtbl.QueryInterface = (void*)FS_QueryInterface;
    g_fakeSampleVtbl.AddRef = (void*)FS_AddRef;
    g_fakeSampleVtbl.Release = (void*)FS_Release;
    g_fakeSampleVtbl.GetUINT32 = (void*)FS_GetUINT32;
    g_fakeSampleVtbl.GetUINT64 = (void*)FS_GetUINT64;
    g_fakeSampleVtbl.GetCount = (void*)FS_GetCount;
    g_fakeSampleVtbl.GetSampleFlags = FS_GetSampleFlags;
    g_fakeSampleVtbl.SetSampleFlags = FS_SetSampleFlags;
    g_fakeSampleVtbl.GetSampleTime = FS_GetSampleTime;
    g_fakeSampleVtbl.SetSampleTime = FS_SetSampleTime;
    g_fakeSampleVtbl.GetSampleDuration = FS_GetSampleDuration;
    g_fakeSampleVtbl.SetSampleDuration = FS_SetSampleDuration;
    g_fakeSampleVtbl.GetBufferCount = FS_GetBufferCount;
    g_fakeSampleVtbl.GetBufferByIndex = FS_GetBufferByIndex;
    g_fakeSampleVtbl.ConvertToContiguousBuffer = FS_ConvertToContiguousBuffer;
    g_fakeSampleVtbl.AddBuffer = FS_AddBuffer;
    g_fakeSampleVtbl.RemoveBufferByIndex = FS_RemoveBufferByIndex;
    g_fakeSampleVtbl.RemoveAllBuffers = FS_RemoveAllBuffers;
    g_fakeSampleVtbl.GetTotalLength = FS_GetTotalLength;
    g_fakeSampleVtbl.CopyToBuffer = FS_CopyToBuffer;

    /* MediaType vtable */
    memset(&g_fakeMediaTypeVtbl, 0, sizeof(g_fakeMediaTypeVtbl));
    g_fakeMediaTypeVtbl.QueryInterface = FMT_QueryInterface;
    g_fakeMediaTypeVtbl.AddRef = FMT_AddRef;
    g_fakeMediaTypeVtbl.Release = FMT_Release;
    g_fakeMediaTypeVtbl.GetItem = FMT_GetItem;
    g_fakeMediaTypeVtbl.GetUINT32 = FMT_GetUINT32;
    g_fakeMediaTypeVtbl.GetUINT64 = FMT_GetUINT64;
    g_fakeMediaTypeVtbl.GetGUID = FMT_GetGUID;
    g_fakeMediaTypeVtbl.GetMajorType = FMT_GetMajorType;
    g_fakeMediaTypeVtbl.IsCompressedFormat = FMT_IsCompressedFormat;
    void **vptr = (void**)&g_fakeMediaTypeVtbl;
    for (int i = 0; i < (int)(sizeof(FakeMediaTypeVtbl)/sizeof(void*)); i++)
        if (!vptr[i]) vptr[i] = (void*)FMT_Stub;

    /* SourceReader vtable */
    g_fakeSourceReaderVtbl.QueryInterface = FSR_QueryInterface;
    g_fakeSourceReaderVtbl.AddRef = FSR_AddRef;
    g_fakeSourceReaderVtbl.Release = FSR_Release;
    g_fakeSourceReaderVtbl.GetStreamSelection = FSR_GetStreamSelection;
    g_fakeSourceReaderVtbl.SetStreamSelection = FSR_SetStreamSelection;
    g_fakeSourceReaderVtbl.GetNativeMediaType = FSR_GetNativeMediaType;
    g_fakeSourceReaderVtbl.GetCurrentMediaType = FSR_GetCurrentMediaType;
    g_fakeSourceReaderVtbl.SetCurrentMediaType = FSR_SetCurrentMediaType;
    g_fakeSourceReaderVtbl.SetCurrentPosition = FSR_SetCurrentPosition;
    g_fakeSourceReaderVtbl.ReadSample = FSR_ReadSample;
    g_fakeSourceReaderVtbl.Flush = FSR_Flush;
    g_fakeSourceReaderVtbl.GetServiceForStream = FSR_GetServiceForStream;
    g_fakeSourceReaderVtbl.GetPresentationAttribute = FSR_GetPresentationAttribute;

    /* Initialize black frame */
    InitBlackFrame();

    /* Initialize pools */
    for (int i = 0; i < MAX_SAMPLES; i++) {
        g_buffers[i].lpVtbl = &g_fakeMediaBufferVtbl;
        g_buffers[i].refCount = 1;
        g_buffers[i].data = g_blackFrame;
        g_buffers[i].dataSize = NV12_SIZE;
        g_samples[i].lpVtbl = &g_fakeSampleVtbl;
        g_samples[i].refCount = 1;
        g_samples[i].sampleTime = 0;
        g_samples[i].sampleDuration = FRAME_DURATION;
        g_samples[i].buffer = &g_buffers[i];
    }

    for (int i = 0; i < MAX_READERS; i++) {
        g_mediaTypes[i].lpVtbl = &g_fakeMediaTypeVtbl;
        g_mediaTypes[i].refCount = 1;
        g_readers[i].lpVtbl = &g_fakeSourceReaderVtbl;
        g_readers[i].refCount = 1;
        g_readers[i].mediaType = &g_mediaTypes[i];
        g_readers[i].readCount = 0;
        g_readers[i].id = i + 1;
        g_readers[i].video.nv12File = INVALID_HANDLE_VALUE;
        for (int j = 0; j < MAX_SAMPLES; j++)
            g_readers[i].video.frameBufs[j] = NULL;
        g_readers[i].video.numFrames = DEFAULT_NUM_FRAMES;
        g_readers[i].video.duration100ns = DEFAULT_DURATION;
        g_readers[i].video.readerNum = i + 1;
        g_readers[i].video.asfSize = 0;
    }
}

/* ============ DLL Entry Point ============ */

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        LogInit();
        Log("DllMain: DLL_PROCESS_ATTACH");
        InitVtables();
        Log("DllMain: initialized, %d readers, %d known videos", MAX_READERS, NUM_KNOWN_VIDEOS);
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("DllMain: DLL_PROCESS_DETACH");
        if (g_logFile) { fflush(g_logFile); fclose(g_logFile); g_logFile = NULL; }
    }
    return TRUE;
}

/* ============ Byte Stream Dump ============ */

typedef HRESULT (STDMETHODCALLTYPE *BS_GetLength)(void *This, ULONGLONG *pqwLength);
typedef HRESULT (STDMETHODCALLTYPE *BS_GetCurrentPosition)(void *This, ULONGLONG *pqwPosition);
typedef HRESULT (STDMETHODCALLTYPE *BS_SetCurrentPosition)(void *This, ULONGLONG qwPosition);
typedef HRESULT (STDMETHODCALLTYPE *BS_Read)(void *This, BYTE *pb, ULONG cb, ULONG *pcbRead);

static ULONGLONG DumpByteStream(void *pByteStream, LONG readerNum) {
    void **vtbl = *(void ***)pByteStream;
    BS_GetLength fnGetLength = (BS_GetLength)vtbl[4];
    BS_GetCurrentPosition fnGetPos = (BS_GetCurrentPosition)vtbl[6];
    BS_SetCurrentPosition fnSetPos = (BS_SetCurrentPosition)vtbl[7];
    BS_Read fnRead = (BS_Read)vtbl[9];

    ULONGLONG curPos = 0;
    fnGetPos(pByteStream, &curPos);

    ULONGLONG length = 0;
    HRESULT hr = fnGetLength(pByteStream, &length);
    if (FAILED(hr) || length == 0 || length > 200 * 1024 * 1024) {
        Log("DumpByteStream R%ld: length=%llu (skip)", readerNum, length);
        return length;
    }

    Log("DumpByteStream R%ld: length=%llu pos=%llu", readerNum, length, curPos);

    fnSetPos(pByteStream, 0);

    BYTE *buf = (BYTE *)VirtualAlloc(NULL, (SIZE_T)length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return length;

    ULONG totalRead = 0;
    while (totalRead < (ULONG)length) {
        ULONG cbRead = 0;
        ULONG toRead = (ULONG)length - totalRead;
        if (toRead > 65536) toRead = 65536;
        hr = fnRead(pByteStream, buf + totalRead, toRead, &cbRead);
        if (FAILED(hr) || cbRead == 0) break;
        totalRead += cbRead;
    }

    char filename[128];
    sprintf(filename, "C:\\movie_%ld.bin", readerNum);
    FILE *f = fopen(filename, "wb");
    if (f) {
        fwrite(buf, 1, totalRead, f);
        fclose(f);
        Log("DumpByteStream R%ld: saved %s (%lu bytes)", readerNum, filename, (unsigned long)totalRead);
    }

    if (totalRead >= 32) {
        char hex[97];
        for (int i = 0; i < 32; i++) sprintf(hex + i*3, "%02X ", buf[i]);
        hex[96] = 0;
        Log("DumpByteStream R%ld: header: %s", readerNum, hex);
    }

    fnSetPos(pByteStream, curPos);
    VirtualFree(buf, 0, MEM_RELEASE);
    return length;
}

/* ============ Exported functions ============ */

HRESULT WINAPI MFCreateSourceReaderFromByteStream(void *pByteStream, void *pAttributes, void **ppSourceReader) {
    LONG idx = InterlockedIncrement(&g_nextReader) - 1;
    if (idx >= MAX_READERS) idx = idx % MAX_READERS;

    FakeSourceReader *reader = &g_readers[idx];
    reader->refCount = 1;
    reader->readCount = 0;

    /* Clean up previous video state if reusing slot */
    if (reader->video.nv12File != INVALID_HANDLE_VALUE) {
        CloseHandle(reader->video.nv12File);
        reader->video.nv12File = INVALID_HANDLE_VALUE;
    }
    for (int i = 0; i < MAX_SAMPLES; i++) {
        if (reader->video.frameBufs[i]) {
            VirtualFree(reader->video.frameBufs[i], 0, MEM_RELEASE);
            reader->video.frameBufs[i] = NULL;
        }
    }

    /* Dump byte stream and get its size */
    ULONGLONG asfSize = DumpByteStream(pByteStream, reader->id);
    reader->video.asfSize = asfSize;
    reader->video.readerNum = reader->id;

    /* Look up video parameters by ASF size */
    const KnownVideo *kv = LookupVideo(asfSize);
    if (kv) {
        reader->video.numFrames = kv->numFrames;
        reader->video.duration100ns = kv->duration100ns;
        Log("R%ld: Known video: \"%s\" (%lu frames, %lldms)",
            reader->id, kv->name, kv->numFrames, kv->duration100ns / 10000);
    } else {
        reader->video.numFrames = DEFAULT_NUM_FRAMES;
        reader->video.duration100ns = DEFAULT_DURATION;
        Log("R%ld: Unknown video (asfSize=%llu), using defaults (%lu frames)",
            reader->id, asfSize, DEFAULT_NUM_FRAMES);
    }

    /* Allocate per-sample frame buffers (ring of MAX_SAMPLES) */
    int allocOk = 1;
    for (int i = 0; i < MAX_SAMPLES; i++) {
        reader->video.frameBufs[i] = (BYTE*)VirtualAlloc(NULL, NV12_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!reader->video.frameBufs[i]) allocOk = 0;
    }
    if (!allocOk) {
        Log("R%ld: FAILED to allocate frame buffers! (%d x %d bytes)", reader->id, MAX_SAMPLES, NV12_SIZE);
    } else {
        Log("R%ld: Allocated %d frame buffers (%d bytes each, %.1fMB total)",
            reader->id, MAX_SAMPLES, NV12_SIZE, (double)MAX_SAMPLES * NV12_SIZE / (1024.0 * 1024.0));
    }

    /* Try to open the NV12 file immediately (might already exist from a previous run) */
    TryOpenNV12(reader);

    *ppSourceReader = reader;
    Log("MFCreateSourceReaderFromByteStream -> S_OK (R%ld, byteStream=%p)", reader->id, pByteStream);
    return S_OK;
}

HRESULT WINAPI MFCreateSourceReaderFromMediaSource(void *a, void *b, void **c) {
    Log("MFCreateSourceReaderFromMediaSource called");
    if (c) *c = NULL;
    return E_NOTIMPL;
}
HRESULT WINAPI MFCreateSourceReaderFromURL(void *a, void *b, void **c) {
    Log("MFCreateSourceReaderFromURL called");
    if (c) *c = NULL;
    return E_NOTIMPL;
}
HRESULT WINAPI MFCreateSinkWriterFromMediaSink(void *a, void *b, void **c) {
    Log("MFCreateSinkWriterFromMediaSink called");
    if (c) *c = NULL;
    return E_NOTIMPL;
}
HRESULT WINAPI MFCreateSinkWriterFromURL(void *a, void *b, void **c) {
    Log("MFCreateSinkWriterFromURL called");
    if (c) *c = NULL;
    return E_NOTIMPL;
}
