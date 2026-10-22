#include "MainScreen.hpp"
#include "ProgressScreen.hpp"
#include "AccountScreen.hpp"
#include "LogScreen.hpp"

#include "../title.hpp"
#include "../savedata.hpp"
#include "../remote.hpp"
#include "../utils.hpp"
#include "../fileio.hpp"


namespace gui
{

MainScreen::MainScreen(Config& config)
    : config(config)
{
}


void MainScreen::resolveAccount()
{
    bool useSelector = (bool)config["account"]["useProfileSelector"];
    std::string defaultName = config["account"]["defaultAccountName"].value;

    // useProfileSelector=0 이고 유효한 defaultAccountName이 있으면 선택창 없이 매칭
    if (!useSelector && !defaultName.empty())
    {
        AccountResolveOptions opts;
        opts.defaultAccountName = defaultName;
        opts.useProfileSelector = false;

        if (getCurrentAccount(&account, opts) == 0)
        {
            accountResolved = true;
            rebuildMenu();
        }
        // 매칭 실패해도 선택창 안 띄움 (accountResolved=false 상태로 에러 표시)
        return;
    }

    // useProfileSelector=1 이거나, defaultAccountName이 비어있으면 선택창
    // full application mode에서는 psel 먼저 시도
    const AppletType appletType = appletGetAppletType();
    const bool isFullApp =
        appletType == AppletType_Application ||
        appletType == AppletType_SystemApplication;

    if (isFullApp)
    {
        AccountResolveOptions opts;
        opts.defaultAccountName = defaultName;
        opts.useProfileSelector = true;

        if (getCurrentAccount(&account, opts) == 0)
        {
            accountResolved = true;
            rebuildMenu();
            return;
        }
    }

    // applet mode이거나 psel 실패: 내장 계정 선택 화면
    App::instance().pushScreen(new AccountScreen([this](const Account& selected)
    {
        onAccountSelected(selected);
    }));
}


void MainScreen::onAccountSelected(const Account& selected)
{
    account = selected;
    accountResolved = true;
    rebuildMenu();
}


void MainScreen::rebuildMenu()
{
    menuItems.clear();
    selectedIndex = 0;

    if (accountResolved)
    {
        bool remoteEnabled = (bool)config["remote"]["enabled"];

        menuItems.push_back({"Push to Server", [this]() { startPush(); }, remoteEnabled});
        menuItems.push_back({"Pull from Server", [this]() { startPull(); }, true});

        // 첫 설치만 사용자가 직접 고르게 한다. 부팅 때 도는 프로세스가
        // 생기는 일이라 몰래 해서는 안 된다.
        const sysmodule::State state = sysmodule::getState();

        if (state == sysmodule::State::NotInstalled)
            menuItems.push_back({"Install background service", [this]() { installSysmodule(); }, true});
        else if (state == sysmodule::State::Interrupted)
        {
            // 지난 실행이 끝까지 가지 못해 스스로 꺼져 있다. 모듈이 죽었을
            // 수도, 백업 도중에 콘솔을 껐을 수도 있다. 알아서 되살리지
            // 않는다 - 전자라면 켜는 순간 같은 일이 반복된다.
            menuItems.push_back({"Re-enable background service", [this]() { resumeSysmodule(); }, true});
            menuItems.push_back({"Remove background service", [this]() { uninstallSysmodule(); }, true});
        }
        else
            menuItems.push_back({"Remove background service", [this]() { uninstallSysmodule(); }, true});

        // 설치되지 않았다면 보여줄 로그도 없다.
        if (state != sysmodule::State::NotInstalled)
            menuItems.push_back({"Show service log", []()
            {
                App::instance().pushScreen(new LogScreen());
            }, true});
    }
}


// 이미 설치돼 있는데 NRO 쪽이 더 새것이면 조용히 갱신한다.
// 쓰겠다는 결정은 이미 내려진 상태이고, 앱과 모듈이 어긋나면 곤란하다.
void MainScreen::updateSysmoduleIfOutdated()
{
    if (sysmodule::getState() != sysmodule::State::Outdated) return;

    if (sysmodule::install() == 0)
        statusMessage = "Background service updated. Reboot to apply.";
    else
        statusMessage = "Failed to update background service.";
}


void MainScreen::installSysmodule()
{
    if (sysmodule::install() == 0)
        statusMessage = "Installed. Reboot to activate.";
    else
        statusMessage = "Install failed. Is the SD card writable?";

    rebuildMenu();
}


void MainScreen::uninstallSysmodule()
{
    if (sysmodule::uninstall() == 0)
        statusMessage = "Removed. Reboot to take effect.";
    else
        statusMessage = "Remove failed.";

    rebuildMenu();
}


void MainScreen::resumeSysmodule()
{
    if (sysmodule::resume() == 0)
        statusMessage = "Re-enabled. Reboot to activate.";
    else
        statusMessage = "Could not re-enable. Is the SD card writable?";

    rebuildMenu();
}


void MainScreen::update(u64 kDown)
{
    // 첫 프레임: 계정 해석
    if (!initialSelectDone)
    {
        initialSelectDone = true;
        resolveAccount();
        return;
    }

    if (!accountResolved) return;

    // 계정이 정해진 뒤 한 번만. 계정을 바꿨다고 다시 돌지 않는다.
    if (!autoPushChecked)
    {
        autoPushChecked = true;
        // 갱신이 먼저다. 아래에서 화면을 밀어버리면 돌아오지 않는다.
        updateSysmoduleIfOutdated();
        startAutoPushIfDue();
        return;
    }

    if (kDown & HidNpadButton_AnyUp)
    {
        selectedIndex--;
        if (selectedIndex < 0) selectedIndex = menuItems.size() - 1;
    }
    if (kDown & HidNpadButton_AnyDown)
    {
        selectedIndex++;
        if (selectedIndex >= (int)menuItems.size()) selectedIndex = 0;
    }
    if (kDown & HidNpadButton_A)
    {
        if (!menuItems.empty() && menuItems[selectedIndex].enabled)
        {
            menuItems[selectedIndex].action();
        }
    }
    if (kDown & HidNpadButton_Minus)
    {
        switchAccount();
    }
}


void MainScreen::switchAccount()
{
    const AppletType appletType = appletGetAppletType();
    const bool isFullApplicationMode =
        appletType == AppletType_Application ||
        appletType == AppletType_SystemApplication;

    if (isFullApplicationMode)
    {
        AccountUid uid;
        PselUserSelectionSettings settings = {};
        if (R_SUCCEEDED(pselShowUserSelector(&uid, &settings)))
        {
            AccountProfile profile;
            AccountProfileBase profileBase;
            if (R_SUCCEEDED(accountGetProfile(&profile, uid)))
            {
                if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &profileBase)))
                {
                    strcpy(account.nickname, profileBase.nickname);
                    account.uid = uid;
                    accountResolved = true;
                    rebuildMenu();
                }
                accountProfileClose(&profile);
            }
        }
    }
    else
    {
        App::instance().pushScreen(new AccountScreen([this](const Account& selected)
        {
            onAccountSelected(selected);
        }));
    }
}


void MainScreen::render(Renderer& r)
{
    int x = 80;
    int y = 60;

    r.drawText("micro NX Save Sync", x, y, 32, COLOR_ACCENT);

    {
        const AppletType appletType = appletGetAppletType();
        const bool isFullApp =
            appletType == AppletType_Application ||
            appletType == AppletType_SystemApplication;
        if (!isFullApp)
        {
            int tagW = r.getTextWidth("Applet Mode", 18);
            r.drawText("Applet Mode", r.screenWidth() - x - tagW, y + 8, 18, COLOR_ERROR);
        }
    }

    y += 50;

    r.drawRect(x, y, r.screenWidth() - x * 2, 2, COLOR_ACCENT);
    y += 20;

    if (!accountResolved)
    {
        r.drawText("Waiting for account selection...", x, y, 24, COLOR_DIM);
        return;
    }

    r.drawText(std::string("Account: ") + account.nickname, x, y, 24);
    y += 36;

    bool remoteEnabled = (bool)config["remote"]["enabled"];
    r.drawText(std::string("Remote: ") + (remoteEnabled ? "Enabled" : "Disabled"), x, y, 18, COLOR_DIM);
    y += 28;

    if (remoteEnabled)
    {
        r.drawText(std::string("Server: ") + (std::string)config["remote"]["serverUrl"], x, y, 18, COLOR_DIM);
        y += 28;
    }

    y += 30;

    for (int i = 0; i < (int)menuItems.size(); i++)
    {
        int btnW = r.screenWidth() - x * 2;
        int btnH = 50;

        Color bgColor = (i == selectedIndex) ? COLOR_HIGHLIGHT : COLOR_BUTTON;
        Color textColor = menuItems[i].enabled ? COLOR_TEXT : COLOR_DIM;

        if (!menuItems[i].enabled && i == selectedIndex)
        {
            bgColor = {80, 80, 80, 255};
        }

        r.drawRect(x, y, btnW, btnH, bgColor);
        r.drawText(menuItems[i].label, x + 20, y + 12, 24, textColor);

        y += btnH + 10;
    }

    if (!statusMessage.empty())
    {
        y += 10;
        r.drawText(statusMessage, x, y, 18, COLOR_ACCENT);
    }

    int fy = r.screenHeight() - 50;
    r.drawRect(x, fy - 10, r.screenWidth() - x * 2, 2, {80, 80, 80, 255});
    r.drawText("A: Select    -: Account    +: Exit", x, fy, 18, COLOR_DIM);
}


SyncOptions MainScreen::buildSyncOptions() const
{
    SyncOptions options;
    options.uid = account.uid;
    options.nickname = account.nickname;
    options.saveDataPath = "sdmc:/uNSS/saves/" + toHex(account.uid.uid[0]) + toHex(account.uid.uid[1]);
    options.serverUrl = (std::string)config["remote"]["serverUrl"];
    options.remoteEnabled = (bool)config["remote"]["enabled"];
    options.archiveBy = config["title"]["archiveBy"].value;
    options.restoreBy = config["title"]["restoreBy"].value;
    options.excludedTitleIds = config["title"]["excludedTitleIds"].value;
    options.excludedTitleNames = config["title"]["excludedTitleNames"].value;
    return options;
}


void MainScreen::startPush()
{
    const SyncOptions options = buildSyncOptions();

    auto work = [=](std::function<void(const std::string&)> log) -> int
    {
        return pushAllSaves(options, log);
    };

    App::instance().pushScreen(new ProgressScreen("Push to Server", std::move(work)));
}


// 계정이 정해진 직후 호출된다. 자동 백업이 켜져 있고 마지막 실행에서
// autoPushIntervalHours 가 지났으면 메뉴를 거치지 않고 바로 업로드한다.
void MainScreen::startAutoPushIfDue()
{
    if (!(bool)config["sync"]["autoPushOnLaunch"]) return;
    if (!(bool)config["remote"]["enabled"]) return;

    // 게임이 돌고 있으면 세이브가 열려 있을 수 있다. 그 상태로 뜬 백업은
    // 반쯤 쓰인 파일을 담을 수 있으므로 자동 백업은 미룬다.
    // 수동 "Push to Server" 는 사용자가 알고 누르는 것이라 막지 않는다.
    if (isGameRunning())
    {
        statusMessage = "Game is running - automatic backup postponed.";
        return;
    }

    const SyncOptions options = buildSyncOptions();
    const int intervalHours = atoi(config["sync"]["autoPushIntervalHours"].value.c_str());

    if (!isAutoSyncDue(options.saveDataPath, intervalHours)) return;

    auto work = [=](std::function<void(const std::string&)> log) -> int
    {
        int ret = pushAllSaves(options, log);
        // 실패했다면 시각을 남기지 않는다. 다음 실행에서 다시 시도한다.
        if (ret == 0)
            writeLastAutoSyncTime(options.saveDataPath, time(NULL));
        return ret;
    };

    App::instance().pushScreen(new ProgressScreen("Auto Backup", std::move(work)));
}


void MainScreen::startPull()
{
    const SyncOptions options = buildSyncOptions();

    auto work = [=](std::function<void(const std::string&)> log) -> int
    {
        return pullAllSaves(options, log);
    };

    App::instance().pushScreen(new ProgressScreen("Pull from Server", std::move(work)));
}

} // namespace gui
