// uNSS 백그라운드 자동 백업 sysmodule.
//
// 부팅할 때 Atmosphere 가 띄우고, 그 뒤로는 계속 살아 있는다. 이따금 깨어나
// 세이브가 바뀌었는지 보고, 바뀌었으면 서버로 올린다 - 특히 게임을 막 끝낸
// 직후에. 아무것도 안 바뀌었으면 아무것도 하지 않는다. 몇 주가 걸리든.
//
// 부팅 때 한 번만 도는 설계였을 때는 사실상 돌지 않았다. 스위치는 끄는
// 물건이 아니라 덮는 물건이라, 재부팅이 몇 주에 한 번이기 때문이다.
//
// 복원은 하지 않는다 - 그건 사용자가 GUI 에서 눈으로 보며 결정할 일이다.
//
// 측정 결과 (실기, 22.5.0): 이 프로세스가 쓸 수 있는 주소 공간은 약 14 MiB.
// 프로토타입이 2.3 MiB 를 썼고 mbedTLS 도 문제없이 올라갔다.

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <malloc.h>
#include <string>
#include <vector>

#include <dirent.h>

#include <switch.h>

#include "account.hpp"
#include "fileio.hpp"
#include "http.hpp"
#include "ini.hpp"
#include "sync.hpp"
#include "utils.hpp"


// 절대 함부로 올리지 말 것.
//
// 6 MiB 로 잡았다가 부팅이 망가졌다 (2026-07-31). HID(0100000000000013) 가
// 메모리를 못 받아 죽었고, 콘솔이 2001-0132 로 부팅 루프에 빠졌다.
// 20.0.0 이후 sysmodule 풀은 아주 빠듯하다. 이 프로세스가 크게 잡으면
// 우리 모듈이 안 뜨는 정도가 아니라 시스템 모듈이 같이 죽는다.
//
// 그리고 2 MiB 도 컸다. 2026-08-01, am(0100000000000023) 이 같은 2001-0132
// 로 세 번 죽었다 - 세 번 다 정확히 같은 자리에서 (PC-start = 0x390b4).
// 매번 우리 모듈이 뜨고 30 초 안이었다. 달라진 것은 하나다: 이날부터
// sys-ftpd 가 같은 풀에서 함께 뜬다. 둘이 같이 들어가지 않는다.
//
//   우리:      0x200000 = 2048 KB
//   sys-ftpd:  0x0A7000 =  668 KB   (그쪽 HEAP_SIZE)
//
// 그래서 절반으로 줄인다. 이 값은 쓰는 만큼이 아니라 잡은 만큼 그대로
// 풀에서 빠진다 - 정적 배열이기 때문이다. 한 바퀴에 실제로 얼마나 쓰는지는
// 이제 로그에 남으므로 (logHeapUsage), 다음에는 재고 나서 정하면 된다.
//
// 모자라면 압축이나 업로드가 실패할 뿐 부팅은 멀쩡하다. 그쪽이 안전하다.
//
// 2026-08-01 에 재봤다. 1 MiB 로 한 바퀴 돌린 뒤 최고점이 189 KB 였다:
//
//   heap after round: 179 KB in use, 189 KB reached, 1024 KB total
//
// 그래서 256 KB 로 줄였다가 되돌렸다. 그 크기에서는 이렇게 나왔다:
//
//   heap after round: 201 KB in use, 245 KB reached,  256 KB total
//
// 같은 일을 하는데 최고점이 189 에서 245 로 올랐다. 최고 사용량은 고정된
// 수치가 아니라 힙 크기에 딸려 움직인다 - 좁으면 조각이 나서 더 쓴다.
// 결과는 압축 실패 (ret=-3), 타이틀 이름 조회 실패 ("Unknown", 144 KB 짜리
// 구조체를 못 잡는다), 업로드 실패 (http=-7) 였다.
//
// 재는 것은 옳았지만 그 값을 하한으로 쓴 것이 틀렸다. 실측치는 참고일 뿐이다.
// 한 바퀴가 끝까지 돌아간 것이 확인된 크기로 돌아간다. 풀에도 여유가 생겼다 -
// 오버레이 로더 세 개를 걷어내고 나서 시작 시점 여유가 15 MB 에서 21 MB 가
// 되었다.
#define INNER_HEAP_SIZE 0x100000

static const char* LOG_PATH = "sdmc:/uNSS/sysmodule.log";
static const char* CONFIG_PATH = "sdmc:/uNSS/config.ini";
static const char* SAVE_DATA_PATH = "sdmc:/uNSS/saves";

// 우리 자신의 부팅 플래그. 스스로를 꺼야 할 때만 건드린다 (RunMarker 참고).
static const char* BOOT_FLAG_PATH =
    "sdmc:/atmosphere/contents/4200000000554E53/flags/boot2.flag";
static const char* BOOT_FLAG_DISABLED_PATH =
    "sdmc:/atmosphere/contents/4200000000554E53/flags/boot2.flag.crashed";

// 실행 중임을 남기는 표시. 안에는 시작 시점의 fatal report 개수가 들어간다.
static const char* RUN_MARKER_PATH = "sdmc:/uNSS/.running";

// 연속으로 몇 번 죽었는지. 백업을 얼마나 쉬었다 다시 해볼지가 여기서 나온다.
//
// 스스로를 영영 끄지는 않는다. 끄는 설계는 "사람이 알아채고 앱을 열어 되살린다"
// 를 전제하는데, 이 콘솔에는 알림이라는 것이 없다. 알아챌 방법이 없는 상태를
// 만들면 백업은 조용히 멈춘 채로 남는다 - 백업이 하지 말아야 할 단 하나다.
//
// 대신 물러섰다 다시 온다. 죽은 자리가 백업이라면 백업만 쉬면 되고, 모듈은
// 계속 살아 있으니 다음 기회에 스스로 복구한다. 사람 손이 필요 없다.
//
// fatal report 개수는 콘솔 전체를 세므로 남의 게임이 죽어도 우리 탓으로
// 보인다. 백업 한 바퀴는 몇 분씩 걸리니 (waitForNetwork 만 최대 180 초)
// 그 확률이 낮지 않다 - 그래서 첫 사고는 짧게만 쉰다.
static const char* CRASH_STRIKE_PATH = "sdmc:/uNSS/.crashes";

// 사고 뒤에 쉬는 바퀴 수. 한 바퀴는 5 분이다.
//
//   1 회  ->  1 바퀴   (5 분)
//   2 회  ->  4 바퀴   (20 분)
//   3 회  -> 16 바퀴   (1 시간 20 분)
//   그 뒤 -> 24 바퀴   (2 시간, 상한)
//
// 첫 번째를 짧게 두는 이유가 있다. fatal report 개수는 콘솔 전체를 세므로,
// 우리가 도는 몇 분 사이에 남의 게임이 죽어도 우리 탓으로 보인다. 그런
// 우연 하나에 몇 시간을 쉬는 것은 과하다. 반대로 우리 버그라면 매번 같은
// 자리에서 재현되므로 금세 위 칸으로 올라간다.
//
// 목표는 이 값을 쓸 일이 없는 것이다. 실제 원인은 고쳤다 - 16KB 스택에
// 144KB 구조체를 올린 것이었고, fatal report 두 개가 같은 자리를 가리켰다.
// 다만 시스템 모듈이 죽으면 자기만 죽는 것이 아니라 콘솔이 같이 죽는다.
// 모르는 이유로 또 죽더라도 5 분마다 콘솔이 꺼지지는 않게 물러서 있을 뿐,
// 끄지는 않는다.
static const int CRASH_BACKOFF_MAX_ROUNDS = 24;

int crashBackoffRounds(int strikes)
{
    if (strikes < 1) return 0;

    int rounds = 1;
    for (int i = 1; i < strikes; ++i)
    {
        if (rounds >= CRASH_BACKOFF_MAX_ROUNDS / 4) return CRASH_BACKOFF_MAX_ROUNDS;
        rounds *= 4;
    }

    return rounds > CRASH_BACKOFF_MAX_ROUNDS ? CRASH_BACKOFF_MAX_ROUNDS : rounds;
}

// Atmosphere 는 사고를 세 군데에 나눠 적는다. 우리가 세는 것은 앞의 두
// 곳뿐이고 - 우리가 겪은 두 번의 사고가 각각 다른 쪽에 적혔다 - 세 번째는
// 셀 수 없다. 그래서 "새 리포트 없음" 을 "아무 일 없었음" 으로 적으면 안 된다.
//
// crash_reports: 일반 프로그램이 죽으면 여기다. 파일 이름에 프로그램 ID 가
//   들어가므로 (예: 01785498797_4200000000554e53.log) 우리 것만 골라 셀 수
//   있다. 우리가 죽었다는 확실한 증거다. 2168-0002 가 여기 있었다.
//
// fatal_reports: 시스템 프로세스가 죽으면 여기다. 이름에는 죽은 쪽의 ID 가
//   들어가므로 우리 것으로 보이지 않는다 - 우리가 남을 죽였을 때가 그렇다.
//   부팅 직후 리소스를 잡아 hid 를 죽였던 2001-0132 가 여기 있었고, 이름은
//   0100000000000013 (hid) 이었다. 그래서 이쪽은 전체 개수로만 볼 수 있고,
//   남의 사고와 구별되지 않는다.
//
// fatal_errors: 세 번째 자리다. 세지 않는다.
//   fatal 모듈 자신이 죽으면 (Title ID 0100000000000034) 리포트가 이쪽으로
//   report_XXXXXXXX.bin 이름으로 가고, 위의 두 디렉터리는 비어 있는 채로
//   남는다. 콘솔이 멈췄는데도 우리 눈에는 아무 흔적이 없는 경우다.
//   실제로 그랬다: 상주 모듈을 하나 더 올렸더니 sm 세션이 동나서
//   (2021-0003, sm::ResultOutOfSessions) 홈브루를 띄울 때마다 콘솔이
//   멈췄는데, 우리 로그는 "아무것도 안 죽었다" 고 적고 있었다
//   (2026-08-02).
//
//   여기를 세지 않는 이유는 우리 사고가 아니기 때문이다. fatal 이 죽은 것은
//   우리가 죽은 것도, 우리가 남을 죽인 것도 아니고, 물러선다고 나아지지도
//   않는다. 대신 위의 두 곳만 봤다는 사실을 로그에 그대로 적는다.
static const char* CRASH_REPORTS_DIR = "sdmc:/atmosphere/crash_reports";
static const char* FATAL_REPORTS_DIR = "sdmc:/atmosphere/fatal_reports";

// crash_reports 파일 이름에서 찾을 우리 프로그램 ID. 소문자로 적힌다.
static const char* OWN_PROGRAM_ID_LOWER = "4200000000554e53";

// 시스템 모듈들이 다 뜰 때까지 비켜서 있는 시간.
// 부팅 직후에 리소스를 잡으면 HID 가 죽는다 (2001-0132).
static const u64 STARTUP_GRACE_SECONDS = 30;

// 한 바퀴 돌고 다음까지 쉬는 시간.
//
// 스위치는 껐다 켜는 물건이 아니라 덮었다 여는 물건이다. 부팅 때 한 번만
// 도는 설계로는 몇 주가 지나도 백업이 돌지 않는다. 그래서 계속 살아 있으면서
// 이따금 들여다본다.
//
// 5 분마다 하는 일은 파일 시각 비교뿐이다. 올릴 것이 없으면 소켓도 열지
// 않는다. 게임을 막 끝냈을 때는 이 간격을 기다리지 않고 바로 확인한다.
static const u64 POLL_SECONDS = 300;

// 게임이 도는 동안에는 더 자주 본다. 끝나는 순간을 놓치면 다음 바퀴까지
// 백업이 밀린다. 이 확인은 pm:dmnt 한 번 호출이라 값이 싸다.
static const u64 GAME_POLL_SECONDS = 60;


extern "C" {

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

extern void __libnx_init_time(void);

static bool g_socketReady = false;
static bool g_nifmReady = false;
static bool g_lateServicesReady = false;
static bool g_pmdmntReady = false;

void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

void __appInit(void)
{
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc))
    {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = timeInitialize();
    if (R_SUCCEEDED(rc))
        __libnx_init_time();

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    fsdevMountSdmc();

    // 여기서 끝낸다. ns / account / socket / nifm 은 열지 않는다.
    //
    // boot2 는 HID 같은 시스템 모듈과 같은 시점에 뜨고, 리소스 풀을 나눠 쓴다.
    // 예전에는 이 자리에서 넷까지 다 열었고, 그 결과가 HID 의 2001-0132
    // (커널 LimitReached) 였다 - 콘솔이 부팅 루프에 빠졌다 (2026-07-31,
    // fatal_reports 로 확인). 우리 로그의 시작 시각과 리포트 시각이 정확히
    // 겹친다.
    //
    // 무거운 것은 initLateServices() 에서 시스템이 다 뜬 뒤에 연다.
    // smExit() 도 그래서 여기서 부르지 않는다 - 나중에 열려면 SM 이 필요하다.
}


// 타이틀 목록과 세이브 시각을 보는 데 필요한 것들. 네트워크는 아직 열지
// 않는다 - 올릴 것이 있는지는 파일 시각만으로 알 수 있고, 없으면 소켓을
// 여는 것 자체가 낭비다.
static bool initTitleServices(void)
{
    if (R_FAILED(nsInitialize()))
        return false;

    if (R_FAILED(accountInitialize(AccountServiceType_System)))
    {
        nsExit();
        return false;
    }
    g_lateServicesReady = true;

    // isGameRunning() 이 쓰는 pm:dmnt 는 SM 이 살아 있을 때 열어야 한다.
    // 그 함수는 자기가 열고 닫지만, smExit() 뒤에는 smGetService 가 실패하고
    // 실패하면 조용히 false - 즉 "게임 안 돌고 있음" - 를 준다. 그러면 게임이
    // 세이브를 붙잡고 있는 채로 백업이 돌아간다.
    //
    // libnx 는 세션을 세므로, 여기서 한 번 열어두면 그 뒤의 initialize /
    // exit 쌍은 카운트만 오르내리고 세션은 살아 있다.
    g_pmdmntReady = R_SUCCEEDED(pmdmntInitialize());

    return true;
}


// 올릴 것이 정말 있을 때만 연다. 소켓 버퍼는 시스템 풀에서 나오고,
// 그 풀을 부팅 직후에 건드린 것이 2001-0132 의 원인이었다.
static bool initNetworkServices(void)
{
    // 프로토타입에서 실기 검증된 작은 값. 키우면 그만큼 풀을 먹는다.
    static const SocketInitConfig sockConf = {
        .tcp_tx_buf_size     = 0x2000,
        .tcp_rx_buf_size     = 0x4000,
        .tcp_tx_buf_max_size = 0x8000,
        .tcp_rx_buf_max_size = 0x10000,
        .udp_tx_buf_size     = 0x800,
        .udp_rx_buf_size     = 0x1000,
        .sb_efficiency       = 1,
        .num_bsd_sessions    = 2,
        .bsd_service_type    = BsdServiceType_User,
    };

    g_socketReady = R_SUCCEEDED(socketInitialize(&sockConf));
    g_nifmReady = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));

    // 여기서 smExit() 를 부르면 안 된다. 네트워크는 소켓만으로 되지 않는다.
    //
    // 이름 풀이부터가 그렇다. sfdnsres 에는 초기화 함수 자체가 없고, 요청
    // 하나하나가 smGetServiceOriginal 로 직접 서비스를 연다 (libnx.a 의
    // sfdnsres.o - 정의된 것은 *Request 뿐이다). socketInitialize 는 bsd 만
    // 열 뿐 sfdnsres 는 건드리지 않는다. SM 을 닫으면 getaddrinfo 가
    // 그때부터 실패한다. TLS 도 마찬가지다 - libcurl 은 첫 https 연결에서야
    // ssl 을 연다.
    //
    // 증상이 고약했다. 소켓은 열려 있으니 "network ready" 가 찍히고, 그
    // 다음 push 만 조용히 떨어진다. 서버 로그에는 아무것도 남지 않는다 -
    // 바이트가 나간 적이 없기 때문이다. 앱에서는 같은 코드가 잘 도는데,
    // 앱은 SM 을 닫지 않아서다 (2026-08-01, http=-4 로 확인).
    //
    // 서비스를 하나씩 미리 열어두는 길도 있지만, 그것은 무엇이 게을리
    // 열리는지 전부 알아야만 맞는 방법이다. 세션 하나를 계속 쥐고 있는
    // 편이 싸고, 무엇보다 다음에 또 틀리지 않는다.

    return g_socketReady && g_nifmReady;
}

void __appExit(void)
{
    // 늦게 연 것들은 열렸을 때만 닫는다. initLateServices() 까지 가지 못하고
    // 끝나는 경로가 여러 개 있다 (설정이 꺼져 있거나, 아직 백업할 때가
    // 아니거나).
    if (g_pmdmntReady) pmdmntExit();
    if (g_nifmReady) nifmExit();
    if (g_socketReady) socketExit();
    if (g_lateServicesReady)
    {
        accountExit();
        nsExit();
    }
    fsdevUnmountAll();
    timeExit();
    fsExit();
}

} // extern "C"


namespace
{

void writeLog(const std::string& line)
{
    FILE* fp = fopen(LOG_PATH, "a");
    if (!fp) return;

    const time_t now = time(NULL);
    struct tm* tm = localtime(&now);

    if (tm)
    {
        fprintf(fp, "[%02d:%02d:%02d] %s\n",
            tm->tm_hour, tm->tm_min, tm->tm_sec, line.c_str());
    }
    else
    {
        fprintf(fp, "%s\n", line.c_str());
    }

    fclose(fp);
}


// 힙을 실제로 얼마나 썼는지 남긴다.
//
// INNER_HEAP_SIZE 는 지금까지 두 번 다 짐작으로 정했고, 두 번 다 너무 컸다:
// 6 MiB 는 hid 를, 2 MiB 는 am 을 죽였다. 둘 다 2001-0132 였고, 둘 다
// "이 정도면 되겠지" 에서 나왔다. 재고 나서 정하면 그럴 일이 없다.
//
// uordblks 는 지금 잡혀 있는 양, arena 는 힙이 자라난 최고점이다. 후자가
// 한 바퀴의 최대 사용량에 가깝다 - 그 값에 여유를 더한 것이 맞는 크기다.
void logHeapUsage(const char* when)
{
    const struct mallinfo mi = mallinfo();

    writeLog(std::string("heap ") + when + ": "
        + std::to_string(mi.uordblks / 1024) + " KB in use, "
        + std::to_string(mi.arena / 1024) + " KB reached, "
        + std::to_string(INNER_HEAP_SIZE / 1024) + " KB total");
}


// 어느 통이 비었는지 커널에 직접 물어본다.
//
// 2001-0132 는 "한계에 닿았다" 는 뜻이고, 커널이 세는 한계는 다섯 가지뿐이다:
// 물리 메모리, 스레드, 이벤트, 전송 메모리, 세션. 지금까지 우리는 그중
// 메모리라고 짐작하고 크기를 두 번 줄였다 (6 -> 2 -> 1 MiB). 두 번 다 am 은
// 똑같이 죽었다. 짐작이 두 번 빗나갔으면 세 번째도 짐작할 일이 아니다.
//
// 값은 줄마다 파일을 닫는 로그로 나가므로, 바로 뒤에 콘솔이 죽어도 남는다.
void logResourceLimits(const char* when)
{
    static const char* POOL_NAMES[] = {"application", "applet", "system", "system-unsafe"};

    for (u64 pool = 0; pool < 4; ++pool)
    {
        u64 total = 0;
        u64 used = 0;

        const Result rcTotal = svcGetSystemInfo(&total, 0, INVALID_HANDLE, pool);
        const Result rcUsed = svcGetSystemInfo(&used, 1, INVALID_HANDLE, pool);

        if (R_FAILED(rcTotal) || R_FAILED(rcUsed))
        {
            writeLog(std::string("pool ") + POOL_NAMES[pool] + ": cannot read (rc="
                + std::to_string(R_FAILED(rcTotal) ? rcTotal : rcUsed) + ")");
            continue;
        }

        writeLog(std::string("pool ") + POOL_NAMES[pool] + " " + when + ": "
            + std::to_string(used / 1024) + " of "
            + std::to_string(total / 1024) + " KB used, "
            + std::to_string((total - used) / 1024) + " KB free");
    }

    // 우리 프로세스가 실제로 쥐고 있는 양. 풀에서 우리 몫이 얼마인지는
    // 이것으로만 알 수 있다 - mallinfo 는 malloc 한 것만 세므로 코드와
    // 스택, 그리고 정적 배열인 힙 자체가 빠져 있다.
    u64 totalMem = 0;
    u64 usedMem = 0;

    if (R_SUCCEEDED(svcGetInfo(&totalMem, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))
        && R_SUCCEEDED(svcGetInfo(&usedMem, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)))
    {
        writeLog(std::string("process memory ") + when + ": "
            + std::to_string(usedMem / 1024) + " of "
            + std::to_string(totalMem / 1024) + " KB used");
    }

    // 우리에게 걸린 한계. 시스템 모듈들은 이것을 나눠 쓴다 - 여기서 우리가
    // 축내면 남이 못 쓰고, 못 쓰는 쪽이 죽는다. am 이 그랬을 수 있다.
    //
    // 아래 다섯 가지 중 sessions 는 특히 값이 나간다. 세션이 동나면
    // 홈브루가 아예 뜨지 않고 콘솔이 멈추는데 (2021-0003), 밖에서는 그
    // 숫자를 볼 방법이 없다 - 이 줄이 유일한 창이다.
    //
    // 이 줄은 두 번 틀렸다. 둘 다 조용히 틀려서 오래 갔다.
    //
    // 첫째, InfoType_ResourceLimit 는 9 인데 5 를 넣고 있었다. 5 는
    // InfoType_HeapRegionSize 라서 호출 자체는 성공했고, 핸들 자리에는
    // 힙 영역 크기가 들어왔다. 그래서 아래 다섯 줄이 실기에서 매번 전부
    // "cannot read" 로 나왔다 - 그런데 rcInfo 는 0 이었으므로 왜 그런지는
    // 어디에도 적히지 않았다. 진단하려고 넣은 코드가 진단을 막고 있었다.
    //
    // 둘째, 핸들 자리다. 이 조회만은 CUR_PROCESS_HANDLE 이 아니라
    // INVALID_HANDLE 을 요구한다. 커널이 그렇게 검사한다:
    //
    //   R_UNLESS(handle == ams::svc::InvalidHandle, svc::ResultInvalidHandle());
    //   -- libmesosphere/source/svc/kern_svc_info.cpp
    //
    // InfoType 만 고쳤을 때 실기에서 2001-0114 가 났다 (rc=58369, 즉
    // 0xE401 -> 모듈 1, 설명 114 = svc::ResultInvalidHandle). 고친 줄이
    // 여전히 읽히지 않았고, 그때는 이유를 몰랐다 (2026-08-02).
    u64 handleValue = 0;
    const Result rcInfo = svcGetInfo(&handleValue, InfoType_ResourceLimit,
        INVALID_HANDLE, 0);

    if (R_FAILED(rcInfo))
    {
        writeLog("resource limit: cannot read (rc=" + std::to_string(rcInfo) + ")");
        return;
    }

    const Handle reslimit = (Handle)handleValue;

    static const char* LIMIT_NAMES[] =
        {"memory-KB", "threads", "events", "transfer-memory", "sessions"};

    for (int which = 0; which < 5; ++which)
    {
        s64 limit = 0;
        s64 current = 0;

        const Result rcLimit =
            svcGetResourceLimitLimitValue(&limit, reslimit, (LimitableResource)which);
        const Result rcCurrent = R_SUCCEEDED(rcLimit)
            ? svcGetResourceLimitCurrentValue(&current, reslimit, (LimitableResource)which)
            : rcLimit;

        if (R_FAILED(rcCurrent))
        {
            // rc 를 같이 적는다. 이것이 없어서 다섯 줄이 몇 주 동안 그냥
            // "cannot read" 였고, 원인이 위의 잘못된 InfoType 이라는 것을
            // 로그만 봐서는 알 수 없었다.
            writeLog(std::string("limit ") + LIMIT_NAMES[which] + ": cannot read (rc="
                + std::to_string(rcCurrent) + ")");
            continue;
        }

        // 메모리만 바이트로 나온다. 나머지는 개수다.
        if (which == 0)
        {
            limit /= 1024;
            current /= 1024;
        }

        writeLog(std::string("limit ") + LIMIT_NAMES[which] + " " + when + ": "
            + std::to_string(current) + " of " + std::to_string(limit));
    }

    svcCloseHandle(reslimit);
}


// 위와 같은 것을 한 줄로. 몇 초 간격으로 반복해서 남기기 위한 것이다.
//
// am 은 우리가 뜬 뒤 4-6 초에 죽는데 (2026-08-01, 네 번 모두), 우리는 그때
// 30 초를 자고 있어서 그 순간의 값을 볼 방법이 없었다. 자는 동안에도 계속
// 적으면 죽기 직전까지의 흐름이 남는다 - 어느 통이 어떻게 줄어드는지가
// 한 번의 스냅샷보다 훨씬 많은 것을 말해준다.
void logPoolsBrief(const char* when)
{
    std::string line = std::string("pools ") + when + " (KB free):";

    for (u64 pool = 0; pool < 4; ++pool)
    {
        static const char* SHORT_NAMES[] = {"app", "applet", "sys", "sys-unsafe"};

        u64 total = 0;
        u64 used = 0;

        if (R_FAILED(svcGetSystemInfo(&total, 0, INVALID_HANDLE, pool))
            || R_FAILED(svcGetSystemInfo(&used, 1, INVALID_HANDLE, pool)))
        {
            line += std::string(" ") + SHORT_NAMES[pool] + "=?";
            continue;
        }

        line += std::string(" ") + SHORT_NAMES[pool] + "="
            + std::to_string((total - used) / 1024);
    }

    writeLog(line);
}


// SD 가 올라올 때까지 기다린다. boot2 는 아주 이른 시점에 돌기 때문에
// 한 번 실패했다고 끝내면 아무것도 못 한다.
bool waitForSdCard(int maxSeconds)
{
    for (int i = 0; i < maxSeconds; ++i)
    {
        FILE* fp = fopen(LOG_PATH, "a");
        if (fp)
        {
            fclose(fp);
            return true;
        }
        svcSleepThread(1000000000ULL);
    }
    return false;
}


// 무선랜이 붙을 때까지 기다린다. 실측 16 초였다.
bool waitForNetwork(int maxSeconds)
{
    if (!g_nifmReady) return false;

    for (int i = 0; i < maxSeconds; ++i)
    {
        NifmInternetConnectionType type;
        u32 strength = 0;
        NifmInternetConnectionStatus status;

        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status))
            && status == NifmInternetConnectionStatus_Connected)
        {
            writeLog("network ready after " + std::to_string(i) + " s");
            return true;
        }

        svcSleepThread(1000000000ULL);
    }

    writeLog("no network after " + std::to_string(maxSeconds) + " s - giving up");
    return false;
}


// 이름 안에 우리 프로그램 ID 가 들어 있는지. 대소문자는 가리지 않는다 -
// Atmosphere 는 소문자로 적지만, 그것에 기대고 싶지 않다.
bool nameHasOwnProgramId(const char* name)
{
    const size_t idLen = strlen(OWN_PROGRAM_ID_LOWER);
    const size_t nameLen = strlen(name);
    if (nameLen < idLen) return false;

    for (size_t start = 0; start + idLen <= nameLen; ++start)
    {
        size_t i = 0;
        for (; i < idLen; ++i)
        {
            if (tolower((unsigned char)name[start + i]) != OWN_PROGRAM_ID_LOWER[i])
                break;
        }
        if (i == idLen) return true;
    }

    return false;
}


// 우리 이름이 붙은 crash report 의 개수. 이것이 늘었다면 우리가 죽은 것이다 -
// 추측이 아니라 Atmosphere 가 적어둔 사실이다.
int countOwnCrashReports()
{
    DIR* dir = opendir(CRASH_REPORTS_DIR);
    if (!dir) return 0;

    int count = 0;
    while (const struct dirent* entry = readdir(dir))
    {
        if (entry->d_name[0] == '.') continue;
        if (nameHasOwnProgramId(entry->d_name)) ++count;
    }

    closedir(dir);
    return count;
}


// Atmosphere 가 남긴 치명적 오류 보고서의 개수.
//
// 시각이 아니라 개수를 쓴다. 부팅 직후에는 RTC 가 아직 맞지 않을 수 있어서
// 시각 비교는 믿을 것이 못 된다. 개수는 단조 증가한다.
//
// 이쪽은 시스템 프로세스가 죽은 기록이라 이름으로 우리를 가려낼 수 없다.
// 개수만 본다 - 우리가 남을 죽였을 때 (2001-0132, hid) 잡히는 유일한 길이다.
int countFatalReports()
{
    DIR* dir = opendir(FATAL_REPORTS_DIR);
    if (!dir) return 0;

    int count = 0;
    while (const struct dirent* entry = readdir(dir))
    {
        if (entry->d_name[0] == '.') continue;
        ++count;
    }

    closedir(dir);
    return count;
}


// 스스로 부팅 플래그를 치운다. 다음 부팅부터 이 모듈은 뜨지 않는다.
void disableSelf()
{
    remove(BOOT_FLAG_DISABLED_PATH);
    rename(BOOT_FLAG_PATH, BOOT_FLAG_DISABLED_PATH);
}


// 실행 중임을 남겨두는 표시. 무사히 끝나면 소멸자가 지운다. 죽으면 남는다.
//
// 남아 있는 표시만으로는 무슨 일이 있었는지 알 수 없다. 우리가 시스템을
// 죽인 것일 수도 있고, 백업 도중에 사용자가 콘솔을 끈 것일 수도 있다.
// 그래서 시작할 때의 fatal report 개수를 함께 적어둔다. 다음 실행에서
// 그 수가 늘어 있으면 전자, 그대로면 후자다.
//
// 이 구분이 중요한 이유: 전자라면 다시 뜨는 것 자체가 위험하고, 후자라면
// 아무 일도 없었으니 그냥 계속하면 된다. 사용자가 앱을 열어 뭔가 눌러야만
// 복구되는 설계는 - 앱을 안 열면 - 영영 복구되지 않는다.
// 어느 구간이었는지도 함께 남긴다. 대응이 정반대이기 때문이다.
//
// 백업 중에 죽었다면 백업만 잠시 쉬면 된다 - 콘솔은 멀쩡히 쓸 수 있고,
// 모듈은 살아서 스스로 다시 해본다.
//
// 부팅 중에 죽었다면 다르다. 다시 떠서 또 죽으면 콘솔이 부팅 루프에 빠지고,
// 그때는 SD 카드를 빼는 것 말고 길이 없다. 그래서 그 경우에만 물러난다.
enum class RunPhase
{
    Boot   = 0,
    Backup = 1,
};

struct RunMarker
{
    static void clear() { remove(RUN_MARKER_PATH); }

    static void write(int fatalCount, int ownCrashCount, RunPhase phase)
    {
        FILE* fp = fopen(RUN_MARKER_PATH, "w");
        if (!fp) return;
        fprintf(fp, "%d %d %d", fatalCount, (int)phase, ownCrashCount);
        fclose(fp);
    }

    // 지난번 표시가 남아 있으면 true, 그때 적어둔 값들을 out 에 넣는다.
    // 필드가 모자란 예전 형식도 읽는다 - 없는 것은 보수적으로 채운다.
    static bool read(int* outFatalCount, RunPhase* outPhase, int* outOwnCrashCount)
    {
        FILE* fp = fopen(RUN_MARKER_PATH, "r");
        if (!fp) return false;

        int fatalCount = 0;
        int phase = (int)RunPhase::Boot;
        int ownCrashCount = -1;
        const int fields = fscanf(fp, "%d %d %d", &fatalCount, &phase, &ownCrashCount);
        fclose(fp);

        if (fields < 1) return false;

        *outFatalCount = fatalCount;
        *outPhase = (fields >= 2 && phase == (int)RunPhase::Backup)
            ? RunPhase::Backup : RunPhase::Boot;

        // 예전 형식에는 이 값이 없다. -1 을 넣어두면 아래에서 "비교할 수
        // 없음" 으로 다뤄져 사고로 오인하지 않는다.
        *outOwnCrashCount = (fields >= 3) ? ownCrashCount : -1;
        return true;
    }
};


// 연속 사고 횟수. 한 바퀴를 무사히 끝내면 0 으로 되돌린다.
struct CrashStrikes
{
    static int read()
    {
        FILE* fp = fopen(CRASH_STRIKE_PATH, "r");
        if (!fp) return 0;

        int value = 0;
        if (fscanf(fp, "%d", &value) != 1) value = 0;
        fclose(fp);

        return value < 0 ? 0 : value;
    }

    static void write(int value)
    {
        FILE* fp = fopen(CRASH_STRIKE_PATH, "w");
        if (!fp) return;
        fprintf(fp, "%d", value);
        fclose(fp);
    }

    static void reset() { remove(CRASH_STRIKE_PATH); }
};


// 백업할 계정을 정한다. sysmodule 에는 선택 화면이 없으므로 설정값으로만
// 결정한다.
//
// allAccounts=1: 콘솔에 등록된 모든 사용자를 백업한다. 서버는
//   /users/<닉네임>/ 으로 사용자를 나누므로 서로 섞이지 않는다.
// allAccounts=0 또는 없음: defaultAccountName 하나만.
bool resolveTargets(Config& config, std::vector<Account>& targets)
{
    if ((bool)config["sync"]["allAccounts"])
    {
        Account* list = NULL;
        size_t count = 0;

        if (probeAccounts(&list, &count) != 0 || list == NULL)
        {
            writeLog("failed to list accounts");
            return false;
        }

        for (size_t i = 0; i < count; ++i)
            targets.push_back(list[i]);

        free(list);
        return true;
    }

    Account account{};
    AccountResolveOptions accountOptions;
    accountOptions.defaultAccountName = config["account"]["defaultAccountName"].value;
    accountOptions.useProfileSelector = false;

    if (accountOptions.defaultAccountName.empty())
    {
        writeLog("defaultAccountName is empty - set it in config.ini");
        return false;
    }

    if (getCurrentAccount(&account, accountOptions) != 0)
    {
        // 닉네임 비교는 대소문자를 구분한다. 오타보다 흔한 원인이라 같이 적어둔다.
        writeLog("account not found (case-sensitive): " + accountOptions.defaultAccountName);
        return false;
    }

    targets.push_back(account);
    return true;
}


SyncOptions makeOptions(Config& config, const Account& account)
{
    SyncOptions options;
    options.uid = account.uid;
    options.nickname = account.nickname;
    options.saveDataPath = std::string(SAVE_DATA_PATH) + "/" + toHex(account.uid.uid[0]) + toHex(account.uid.uid[1]);
    options.serverUrl = (std::string)config["remote"]["serverUrl"];
    options.remoteEnabled = true;
    options.archiveBy = config["title"]["archiveBy"].value;
    options.excludedTitleIds = config["title"]["excludedTitleIds"].value;
    options.excludedTitleNames = config["title"]["excludedTitleNames"].value;
    // 바뀐 것만 올린다. 매번 전부 올리면 SD 와 서버를 모두 낭비한다.
    options.skipUnchanged = true;
    options.stopOnUploadError = true;
    return options;
}


// 네트워크는 처음 필요할 때 열고, 그 뒤로는 열어둔다. 여닫기를 반복하면
// 그때마다 시스템 풀에서 버퍼를 다시 잡는다 - 굳이 그럴 이유가 없다.
bool ensureNetwork()
{
    if (!g_socketReady || !g_nifmReady)
    {
        if (!initNetworkServices())
        {
            writeLog("failed to open network services");
            return false;
        }
    }

    return waitForNetwork(180);
}


// 한 바퀴. 올릴 것이 없으면 네트워크도 건드리지 않고 조용히 돌아간다.
void runBackupRound(Config& config, const std::vector<Account>& targets)
{
    int pending = 0;
    for (const Account& account : targets)
    {
        const int changed = countChangedTitles(makeOptions(config, account));
        if (changed > 0) pending += changed;
    }

    if (pending == 0) return;

    writeLog("titles to upload: " + std::to_string(pending));

    logHeapUsage("before network");
    logResourceLimits("before network");
    if (!ensureNetwork())
    {
        writeLog("no network - will try again later");
        return;
    }

    logHeapUsage("after network");
    logResourceLimits("after network");
    bool allOk = true;

    for (const Account& account : targets)
    {
        writeLog(std::string("account: ") + account.nickname);
        logHeapUsage("before account");

        const int ret = pushAllSaves(makeOptions(config, account), [](const std::string& line)
        {
            if (!line.empty() && line[0] == '\r') return;
            writeLog("  " + line);
        });

        logHeapUsage("after account");
        if (ret != 0)
        {
            writeLog("  failed, ret=" + std::to_string(ret));
            allOk = false;
            writeLog("stopping this round after the first error; saves remain pending");
            logResourceLimits("after backup error");
            break;
        }
    }

    // 하나라도 실패하면 시각을 남기지 않는다. 다음 바퀴에서 다시 시도한다.
    if (allOk)
    {
        writeLastAutoSyncTime(SAVE_DATA_PATH, time(NULL));
        writeLog("backup finished");
        logHeapUsage("after round");
    }
    else
    {
        writeLog("backup finished with errors - will retry");
        logHeapUsage("after round");
    }
}

} // namespace


int main(int argc, char* argv[])
{
    if (!waitForSdCard(60))
        return 0;

    writeLog("--- uNSS sysmodule started ---");

    // 아직 아무것도 열지 않은 시점이다. 여기 값이 콘솔의 평소 상태에 가장
    // 가깝고, 아래의 두 번째 측정과 비교하면 우리가 얼마나 축내는지 나온다.
    logResourceLimits("at start");

    // 지난번에 끝까지 가지 못했다면, 우리 탓인지부터 가린다.
    const int fatalNow = countFatalReports();
    const int ownCrashNow = countOwnCrashReports();

    int fatalBefore = 0;
    int ownCrashBefore = -1;
    RunPhase lastPhase = RunPhase::Boot;

    // 0 보다 크면 그만큼의 바퀴 동안 백업을 건너뛴다.
    int backoffRounds = 0;

    if (RunMarker::read(&fatalBefore, &lastPhase, &ownCrashBefore))
    {
        // 우리 이름이 붙은 보고서가 늘었다면 확실히 우리다.
        const bool weCrashed = (ownCrashBefore >= 0) && (ownCrashNow > ownCrashBefore);
        // 시스템 쪽이 늘었다면 우리일 수도, 남일 수도 있다.
        const bool systemCrashed = fatalNow > fatalBefore;

        if (weCrashed || systemCrashed)
        {
            const int strikes = CrashStrikes::read() + 1;
            CrashStrikes::write(strikes);

            writeLog(weCrashed
                ? ("previous run crashed - " + std::to_string(ownCrashNow - ownCrashBefore)
                    + " new crash report(s) with our program id, strike "
                    + std::to_string(strikes))
                : ("previous run ended in a system crash ("
                    + std::to_string(fatalNow - fatalBefore)
                    + " new fatal report(s), strike " + std::to_string(strikes)
                    + ") - could also have been another process"));

            if (lastPhase == RunPhase::Boot)
            {
                // 부팅 구간이다. 다시 떠서 또 죽으면 콘솔이 부팅 루프에
                // 빠지고, 그러면 SD 카드를 빼는 것 말고 길이 없다.
                writeLog("it happened during startup - standing down so the console can boot");
                writeLog("re-enable from the app once the cause is fixed");

                disableSelf();
                remove(RUN_MARKER_PATH);
                return 0;
            }

            // 백업 구간이다. 여기서 갈린다: 우리만 죽었나, 콘솔이 죽었나.
            //
            // 우리만 죽었다면 콘솔은 멀쩡히 쓸 수 있다. 잠시 쉬었다 스스로
            // 다시 해보면 된다 - 사람 손은 필요 없다.
            //
            // fatal report 는 다르다. 시스템 프로세스가 죽었다는 뜻이고,
            // 그러면 콘솔 전체가 빨간 화면으로 멈춘다. "백업 한 번 실패"
            // 와 같은 무게로 다룰 수 없다. 5 분 뒤에 다시 해보다가 또
            // 죽으면 5 분마다 콘솔이 멈추는 물건이 된다 - 실제로 그랬다
            // (2026-08-01, am 이 2001-0132 로 두 번, 273 초 간격).
            //
            // 그래서 fatal 은 처음부터 최대치로 물러선다. 그래도 또 나면
            // 우연이 아니므로 끈다. 백업이 멈추는 쪽이 콘솔이 멈추는 쪽보다
            // 낫고, 앱에서 한 번 눌러 되살릴 수 있다.
            if (systemCrashed && strikes >= 2)
            {
                writeLog("the console itself went down twice - standing down");
                writeLog("re-enable from the app once the cause is fixed");

                disableSelf();
                remove(RUN_MARKER_PATH);
                return 0;
            }

            backoffRounds = systemCrashed
                ? CRASH_BACKOFF_MAX_ROUNDS
                : crashBackoffRounds(strikes);

            writeLog(std::string("it happened during a backup - skipping the next ")
                + std::to_string(backoffRounds) + " round(s) ("
                + std::to_string(backoffRounds * (int)(POLL_SECONDS / 60))
                + " min), then trying again"
                + (systemCrashed ? " - the whole console went down, so backing off all the way" : ""));
        }
        else
        {
            // 표시는 남았지만 우리가 보는 두 디렉터리에는 새 리포트가 없다.
            // 대개는 백업 도중에 콘솔을 껐을 뿐이다.
            //
            // 다만 "새 리포트가 없다" 와 "아무 일도 없었다" 는 다르다.
            // fatal 모듈 자신이 죽으면 (fatal_errors, 아래 설명) 두 곳 다
            // 비어 있는 채로 남는다. 그러니 아는 만큼만 적는다.
            writeLog("previous run did not finish - no new reports in "
                "crash_reports or fatal_reports, continuing");
        }
    }

    // 위험한 구간에 들어가기 전에 표시를 남긴다. 한 바퀴를 무사히 넘기면
    // 지운다 - 부팅 직후가 위험한 구간이고, 그 뒤로는 아니다.
    RunMarker::write(fatalNow, ownCrashNow, RunPhase::Boot);

    // 아예 꺼져 있으면 계속 살아 있을 이유가 없다.
    {
        Config config(CONFIG_PATH);

        if (!(bool)config["remote"]["enabled"])
        {
            writeLog("remote disabled in config - nothing to do");
            RunMarker::clear();
            return 0;
        }
        if (!(bool)config["sync"]["autoPushOnLaunch"])
        {
            writeLog("autoPushOnLaunch is off - nothing to do");
            RunMarker::clear();
            return 0;
        }

        // 기본은 검증 켜짐. 자격증명이 URL 에 들어가는 이상, 검증을 끄면
        // 핸드셰이크에 응답하는 누구나 평문 비밀번호를 받는다.
        const bool skipVerify = (bool)config["remote"]["insecureSkipVerify"];
        HTTPClient::setVerifyTls(!skipVerify);
        if (skipVerify)
            writeLog("WARNING: insecureSkipVerify=1 - the password is exposed to anyone answering the handshake");
    }

    // 여기까지는 파일만 읽었다. 이제부터 서비스를 연다 - 그 전에 시스템이
    // 자리를 잡을 시간을 준다. 백업은 몇 초 늦어도 상관없지만, 시스템 모듈과
    // 리소스를 다투면 콘솔이 부팅 루프에 빠진다.
    // 자는 동안에도 5 초마다 값을 남긴다. 우리가 죽이고 있는 것이 무엇이든,
    // 그 일은 바로 이 구간에서 벌어진다.
    for (u64 elapsed = 0; elapsed < STARTUP_GRACE_SECONDS; elapsed += 5)
    {
        logPoolsBrief(("+" + std::to_string(elapsed) + "s").c_str());
        svcSleepThread(5 * 1000000000ULL);
    }

    if (!initTitleServices())
    {
        writeLog("failed to open system services - aborting");
        RunMarker::clear();
        return 0;
    }

    recursiveMkdir(SAVE_DATA_PATH);
    writeLog("watching for changes");

    // 서비스를 다 연 뒤. 시작 시점과의 차이가 곧 우리 몫이다.
    logResourceLimits("after services");

    bool markerCleared = false;
    bool gameWasRunning = false;

    // 여기서부터는 끝나지 않는다.
    //
    // 스위치는 끄는 물건이 아니라 덮는 물건이다. 부팅 때 한 번만 도는 설계는
    // 몇 주가 지나도 백업을 하지 못한다. 그래서 계속 살아 있으면서, 게임이
    // 끝난 뒤에 바뀐 세이브가 있는지 들여다본다.
    while (true)
    {
        // 설정은 매 바퀴 새로 읽는다. 그래야 config.ini 를 고쳤을 때
        // 재부팅 없이 반영된다.
        Config config(CONFIG_PATH);

        const bool enabled = (bool)config["remote"]["enabled"]
            && (bool)config["sync"]["autoPushOnLaunch"];

        bool gameRunning = false;

        if (enabled)
        {
            gameRunning = isGameRunning();

            if (gameRunning)
            {
                // 게임이 세이브를 붙잡고 있다. 반쯤 쓰인 파일을 올릴 수는 없다.
                if (!gameWasRunning) writeLog("game started - holding off");
                gameWasRunning = true;
            }
            else
            {
                // 게임을 막 끝냈다면 간격을 기다리지 않는다. 사람이 저장하고
                // 나온 직후가 백업하기 가장 좋은 때다.
                const bool justFinished = gameWasRunning;
                gameWasRunning = false;

                if (justFinished) writeLog("game ended - checking saves");

                const int intervalHours =
                    atoi(config["sync"]["autoPushIntervalHours"].value.c_str());

                if (backoffRounds > 0)
                {
                    // 지난번에 백업 도중 죽었다. 끄지는 않되, 5 분마다 같은
                    // 벽에 부딪히지도 않는다. 세어 내려가다 0 이 되면 다시 한다.
                    --backoffRounds;
                    if (backoffRounds == 0)
                        writeLog("backoff over - trying a backup again");
                }
                else if (justFinished || isAutoSyncDue(SAVE_DATA_PATH, intervalHours))
                {
                    std::vector<Account> targets;
                    if (resolveTargets(config, targets) && !targets.empty())
                    {
                        // 부팅 직후만 위험한 것이 아니다. 백업 한 바퀴가 이
                        // 모듈이 하는 일의 전부이고, 죽는다면 십중팔구 그
                        // 안에서 죽는다 - 실제로 그랬다 (2168-0002).
                        //
                        // 처음에는 첫 바퀴를 넘기면 표시를 지웠다. 그래서
                        // 몇 시간 뒤 백업 도중에 죽었을 때 아무도 알아채지
                        // 못했고, 다음 부팅에서 같은 자리에서 또 죽었다.
                        // 이제는 백업할 때마다, 구간까지 적어 남긴다.
                        RunMarker::write(countFatalReports(), countOwnCrashReports(), RunPhase::Backup);
                        runBackupRound(config, targets);
                        RunMarker::clear();

                        // 한 바퀴를 끝까지 돌았다. 지난번 사고가 무엇이었든
                        // 이 자리에서 재현되지 않으므로 누적을 지운다.
                        CrashStrikes::reset();
                    }
                }
            }
        }

        // 부팅 표시를 지운다. 부팅 직후 구간은 넘겼다는 뜻이다. 백업 구간은
        // 위에서 따로 표시하고 지우므로, 여기서 지워도 보호는 남는다.
        if (!markerCleared)
        {
            RunMarker::clear();
            markerCleared = true;
        }

        // 게임 중에는 조금 더 자주 본다. 끝나는 순간을 놓치지 않으려는 것이고,
        // isGameRunning() 자체는 값이 싸다.
        const u64 sleepSeconds = gameRunning ? GAME_POLL_SECONDS : POLL_SECONDS;
        svcSleepThread(sleepSeconds * 1000000000ULL);
    }

    return 0;
}
