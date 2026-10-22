#include "ProgressScreen.hpp"


namespace gui
{

ProgressScreen::ProgressScreen(const std::string& title, WorkFunc work)
    : title(title)
    , work(std::move(work))
{
    logLines.push_back("Starting...");
}


void ProgressScreen::runWork()
{
    started = true;

    auto log = [this](const std::string& msg)
    {
        if (!msg.empty() && msg[0] == '\r' && !logLines.empty() && !logLines.back().empty() && logLines.back()[0] == '\r')
            logLines.back() = msg;
        else
            logLines.push_back(msg);

        // 로그가 추가될 때마다 화면 갱신
        int logTop = 60 + 50 + 15; // title + separator
        int logBottom = 720 - 50 - 20; // status bar
        int maxVisible = (logBottom - logTop) / 26;
        if (maxVisible < 1) maxVisible = 1;
        int maxScroll = (int)logLines.size() - maxVisible;
        if (maxScroll < 0) maxScroll = 0;
        scrollOffset = maxScroll;

        renderNow();
    };

    result = work(log);
    finished = true;

    if (result == 0)
        logLines.push_back("Done.");
    else
        logLines.push_back("Done with errors (code: " + std::to_string(result) + ")");

    logLines.push_back("Press B to go back.");

    int logTop = 60 + 50 + 15;
    int logBottom = 720 - 50 - 20;
    int maxVisible = (logBottom - logTop) / 26;
    if (maxVisible < 1) maxVisible = 1;
    int maxScroll = (int)logLines.size() - maxVisible;
    if (maxScroll < 0) maxScroll = 0;
    scrollOffset = maxScroll;
}


void ProgressScreen::renderNow()
{
    Renderer& r = App::instance().getRenderer();
    r.beginFrame();
    render(r);
    r.endFrame();
}


void ProgressScreen::update(u64 kDown)
{
    // 첫 프레임에서 작업 시작 (화면이 한 번 그려진 후)
    if (!started)
    {
        runWork();
        return;
    }

    if (finished)
    {
        if (kDown & HidNpadButton_B)
        {
            App::instance().popScreen();
            return;
        }
    }

    // scroll
    int logTop = 60 + 50 + 15;
    int logBottom = 720 - 50 - 20;
    int maxVisible = (logBottom - logTop) / 26;
    if (maxVisible < 1) maxVisible = 1;
    int maxScroll = (int)logLines.size() - maxVisible;
    if (maxScroll < 0) maxScroll = 0;

    if (kDown & HidNpadButton_AnyUp)
    {
        scrollOffset--;
        if (scrollOffset < 0) scrollOffset = 0;
    }
    if (kDown & HidNpadButton_AnyDown)
    {
        scrollOffset++;
        if (scrollOffset > maxScroll) scrollOffset = maxScroll;
    }
}


void ProgressScreen::render(Renderer& r)
{
    int x = 80;
    int y = 60;

    r.drawText(title, x, y, 32, COLOR_ACCENT);
    y += 50;

    r.drawRect(x, y, r.screenWidth() - x * 2, 2, COLOR_ACCENT);
    y += 15;

    // status bar (먼저 위치 계산)
    int fy = r.screenHeight() - 50;
    int logAreaBottom = fy - 20;
    int lineHeight = 26;
    int maxVisible = (logAreaBottom - y) / lineHeight;
    if (maxVisible < 1) maxVisible = 1;

    // log lines
    for (int i = scrollOffset; i < (int)logLines.size() && i < scrollOffset + maxVisible; i++)
    {
        r.drawText(!logLines[i].empty() && logLines[i][0] == '\r' ? logLines[i].substr(1) : logLines[i], x, y, 18, COLOR_TEXT);
        y += lineHeight;
    }

    // status bar background (로그가 넘치지 않도록 덮기)
    r.drawRect(0, logAreaBottom, r.screenWidth(), r.screenHeight() - logAreaBottom, COLOR_BACKGROUND);
    r.drawRect(x, fy - 10, r.screenWidth() - x * 2, 2, {80, 80, 80, 255});

    if (finished)
    {
        if (result == 0)
            r.drawText("Done.  B: Back    +: Exit", x, fy, 18, COLOR_ACCENT);
        else
            r.drawText("Done with errors.  B: Back    +: Exit", x, fy, 18, COLOR_ERROR);
    }
    else
    {
        r.drawText("Processing...    +: Exit", x, fy, 18, COLOR_DIM);
    }
}

} // namespace gui
