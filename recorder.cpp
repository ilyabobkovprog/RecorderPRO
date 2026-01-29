#include <wx/wx.h>
#include <wx/filename.h>
#include <wx/spinctrl.h>
#include <wx/datetime.h>
#include <wx/hyperlink.h>
#include <wx/sizer.h>
#include <wx/display.h>
#include <thread>
#include <atomic>
#include <vector>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <algorithm>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

std::atomic<bool> g_IsRecording(false);
std::atomic<bool> g_IsPaused(false);

struct AudioDeviceInfo
{
    ma_device_id id;
    std::string name;
};

void RecordLogic(int delay, std::string folderPath, ma_device_id *pSelectedId, bool mute, bool onlyAudio, int width, int height)
{
    // Создаем вложенную структуру папок records/дата/время
    if (!wxFileName::Mkdir(folderPath, 0777, wxPATH_MKDIR_FULL))
        return;
    if (delay > 0)
        std::this_thread::sleep_for(std::chrono::seconds(delay));

    ma_encoder encoder;
    ma_device device;
    bool audioInitialized = false;

    std::string audioPath = folderPath + "/audio.wav";
    std::string videoPath = folderPath + "/video_silent.mp4";
    std::string finalPath = folderPath + (onlyAudio ? "/audio_only.wav" : "/record.mp4");

    if (!mute) {
        ma_encoder_config encCfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, 16000);
        if (ma_encoder_init_file(audioPath.c_str(), &encCfg, &encoder) == MA_SUCCESS) {
            ma_device_config devCfg = ma_device_config_init(ma_device_type_capture);
            devCfg.capture.pDeviceID = pSelectedId;
            devCfg.capture.format = encoder.config.format;
            devCfg.capture.channels = encoder.config.channels;
            devCfg.sampleRate = encoder.config.sampleRate;
            devCfg.dataCallback = [](ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
            {
                if (!g_IsPaused)
                    ma_encoder_write_pcm_frames((ma_encoder *)pDevice->pUserData, pInput, frameCount, NULL);
            };
            devCfg.pUserData = &encoder;

            if (ma_device_init(NULL, &devCfg, &device) == MA_SUCCESS) {
                ma_device_start(&device);
                audioInitialized = true;
            }
        }
    }

    FILE *pipe = nullptr;
    if (!onlyAudio) {
        std::string videoSize = std::to_string(width) + "x" + std::to_string(height);
        std::string cmd = "ffmpeg -y -loglevel quiet -f x11grab -video_size " + videoSize + " -i :0.0 -c:v libx264 -preset ultrafast " + videoPath;
        pipe = popen(cmd.c_str(), "w");
    }

    while (g_IsRecording)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (audioInitialized) {
        ma_device_uninit(&device);
        ma_encoder_uninit(&encoder);
    }

    if (pipe)
    {
        fprintf(pipe, "q\n");
        pclose(pipe);
    }

    if (onlyAudio) {
        if (audioInitialized) rename(audioPath.c_str(), finalPath.c_str());
    } else {
        if (audioInitialized) {
            std::string mergeCmd = "ffmpeg -y -loglevel quiet -i " + videoPath + " -i " + audioPath + " -c:v copy -c:a aac -map 0:v:0 -map 1:a:0 " + finalPath;
            system(mergeCmd.c_str());
            remove(videoPath.c_str());
            remove(audioPath.c_str());
        } else {
            rename(videoPath.c_str(), finalPath.c_str());
        }
    }
}

class RecorderFrame : public wxFrame
{
public:
    RecorderFrame() : wxFrame(NULL, wxID_ANY, "RECORDER PRO", wxDefaultPosition, wxSize(430, 315))
    {
        SetBackgroundColour(wxColour(240, 240, 240));
        m_panel = new wxPanel(this);
        wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

        wxStaticBoxSizer *settingsBox = new wxStaticBoxSizer(wxVERTICAL, m_panel, wxString::FromUTF8("Настройки захвата"));
        m_deviceChoice = new wxChoice(m_panel, wxID_ANY);
        m_spinDelay = new wxSpinCtrl(m_panel, wxID_ANY, "0");

        settingsBox->Add(new wxStaticText(m_panel, wxID_ANY, wxString::FromUTF8("Режим / Аудио-устройство:")), 0, wxALL, 5);
        settingsBox->Add(m_deviceChoice, 0, wxEXPAND | wxALL, 5);
        settingsBox->Add(new wxStaticText(m_panel, wxID_ANY, wxString::FromUTF8("Задержка на старте (сек):")), 0, wxALL, 5);
        settingsBox->Add(m_spinDelay, 0, wxEXPAND | wxALL, 5);

        wxBoxSizer *btnSizer = new wxBoxSizer(wxHORIZONTAL);
        m_btnStart = new wxButton(m_panel, wxID_ANY, wxString::FromUTF8("⏺ ЗАПИСЬ"));
        m_btnPause = new wxButton(m_panel, wxID_ANY, wxString::FromUTF8("⏸ ПАУЗА"));
        m_btnStop = new wxButton(m_panel, wxID_ANY, wxString::FromUTF8("⏹ СТОП"));

        m_btnStart->SetBackgroundColour(wxColour(180, 255, 180));
        m_btnStop->SetBackgroundColour(wxColour(255, 180, 180));

        btnSizer->Add(m_btnStart, 1, wxALL | wxEXPAND, 5);
        btnSizer->Add(m_btnPause, 1, wxALL | wxEXPAND, 5);
        btnSizer->Add(m_btnStop, 1, wxALL | wxEXPAND, 5);

        wxStaticText *hotkeyHint = new wxStaticText(m_panel, wxID_ANY, wxString::FromUTF8("Горячие клавиши: F10 - Старт/Стоп | F11 - Пауза"));
        hotkeyHint->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        hotkeyHint->SetForegroundColour(wxColour(100, 100, 100));

        m_combinedStatusSizer = new wxBoxSizer(wxHORIZONTAL);
        m_statusTxt = new wxStaticText(m_panel, wxID_ANY, wxString::FromUTF8("Готов к работе"));
        m_statusTxt->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
        m_linkOpenFolder = new wxHyperlinkCtrl(m_panel, wxID_ANY, wxString::FromUTF8("(открыть папку)"), "");
        m_linkOpenFolder->Hide();

        m_combinedStatusSizer->Add(m_statusTxt, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
        m_combinedStatusSizer->Add(m_linkOpenFolder, 0, wxALIGN_CENTER_VERTICAL);

        mainSizer->Add(settingsBox, 0, wxEXPAND | wxALL, 15);
        mainSizer->Add(btnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        mainSizer->Add(hotkeyHint, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 5);
        mainSizer->AddSpacer(5);
        mainSizer->Add(m_combinedStatusSizer, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 10);

        m_btnPause->Enable(false);
        m_btnStop->Enable(false);
        m_panel->SetSizer(mainSizer);

        m_btnStart->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { StartAction(); });
        m_btnPause->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { PauseAction(); });
        m_btnStop->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { StopAction(); });
        m_linkOpenFolder->Bind(wxEVT_HYPERLINK, &RecorderFrame::OnOpenFolder, this);

        RefreshDevices();
        StartGlobalHotkeyThread();
    }

private:
    wxPanel *m_panel;
    wxButton *m_btnStart, *m_btnPause, *m_btnStop;
    wxSpinCtrl *m_spinDelay;
    wxChoice *m_deviceChoice;
    wxStaticText *m_statusTxt;
    wxHyperlinkCtrl *m_linkOpenFolder;
    wxBoxSizer *m_combinedStatusSizer;
    wxString m_currentFolderPath;
    std::vector<AudioDeviceInfo> m_devices;

    void RefreshDevices()
    {
        m_devices.clear();
        m_deviceChoice->Clear();
        int firstMicIndex = -1;
        ma_context context;
        if (ma_context_init(NULL, 0, NULL, &context) == MA_SUCCESS) {
            ma_device_info* pCaptureInfos; ma_uint32 captureCount;
            if (ma_context_get_devices(&context, NULL, NULL, &pCaptureInfos, &captureCount) == MA_SUCCESS) {
                for (ma_uint32 i = 0; i < captureCount; ++i) {
                    AudioDeviceInfo info = { pCaptureInfos[i].id, pCaptureInfos[i].name };
                    m_devices.push_back(info);
                    std::string lowerName = info.name;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                    bool isMonitor = (lowerName.find("monitor") != std::string::npos);
                    wxString prefix = isMonitor ? wxString::FromUTF8("[СИСТЕМА] ") : wxString::FromUTF8("[МИКРОФОН] ");
                    int currentIdx = m_deviceChoice->Append(prefix + wxString::FromUTF8(info.name.c_str()));
                    if (!isMonitor && firstMicIndex == -1) firstMicIndex = currentIdx;
                }
            }
            ma_context_uninit(&context);
        }
        m_deviceChoice->Append(wxString::FromUTF8("Без звука (только видео)"));
        m_deviceChoice->Append(wxString::FromUTF8("Только аудио (без видео)"));
        if (firstMicIndex != -1) m_deviceChoice->SetSelection(firstMicIndex);
        else if (m_deviceChoice->GetCount() > 0) m_deviceChoice->SetSelection(0);
    }

    void StartAction()
    {
        if (g_IsRecording) return;
        int sel = m_deviceChoice->GetSelection();
        int count = m_deviceChoice->GetCount();
        bool onlyAudio = (sel == count - 1);
        bool mute = (sel == count - 2);
        ma_device_id *pId = nullptr;
        if (!mute && !onlyAudio && sel < (int)m_devices.size()) pId = &m_devices[sel].id;

        wxSize screenSize = wxDisplay(wxDisplay::GetFromWindow(this)).GetGeometry().GetSize();
        
        // Формируем путь: records/YYYY-MM-DD/HH-MM-SS
        wxDateTime now = wxDateTime::Now();
        m_currentFolderPath = wxString::Format("records/%s/%s", 
                              now.FormatISODate(), 
                              now.Format("%H-%M-%S"));

        g_IsRecording = true;
        g_IsPaused = false;
        m_statusTxt->SetLabel(wxString::FromUTF8("Идет запись..."));
        m_statusTxt->SetForegroundColour(*wxRED);
        m_linkOpenFolder->Hide();
        m_btnStart->Enable(false);
        m_btnPause->Enable(true);
        m_btnStop->Enable(true);
        m_panel->Layout();

        std::thread(RecordLogic, m_spinDelay->GetValue(), m_currentFolderPath.ToStdString(), pId, mute, onlyAudio, screenSize.x, screenSize.y).detach();
    }

    void StopAction()
    {
        if (!g_IsRecording) return;
        g_IsRecording = false;
        m_statusTxt->SetLabel(wxString::FromUTF8("Запись сохранена!"));
        m_statusTxt->SetForegroundColour(wxColour(0, 150, 0));
        m_linkOpenFolder->SetURL(m_currentFolderPath);
        m_linkOpenFolder->Show();
        m_btnStart->Enable(true);
        m_btnPause->Enable(false);
        m_btnStop->Enable(false);
        m_btnPause->SetLabel(wxString::FromUTF8("⏸ ПАУЗА"));
        m_panel->Layout();
    }

    void PauseAction()
    {
        if (!g_IsRecording) return;
        g_IsPaused = !g_IsPaused;
        if (g_IsPaused) {
            m_statusTxt->SetLabel(wxString::FromUTF8("⏸ Пауза"));
            m_statusTxt->SetForegroundColour(wxColour(255, 140, 0));
            m_btnPause->SetLabel(wxString::FromUTF8("▶ Продолжить"));
        } else {
            m_statusTxt->SetLabel(wxString::FromUTF8("Идет запись..."));
            m_statusTxt->SetForegroundColour(*wxRED);
            m_btnPause->SetLabel(wxString::FromUTF8("⏸ ПАУЗА"));
        }
    }

    void StartGlobalHotkeyThread()
    {
        std::thread([](RecorderFrame* frame) {
            Display* display = XOpenDisplay(NULL);
            if (!display) return;
            Window root = DefaultRootWindow(display);
            XGrabKey(display, XKeysymToKeycode(display, XK_F10), AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
            XGrabKey(display, XKeysymToKeycode(display, XK_F11), AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
            XSelectInput(display, root, KeyPressMask);
            XEvent event;
            while (true) {
                XNextEvent(display, &event);
                if (event.type == KeyPress) {
                    if (event.xkey.keycode == XKeysymToKeycode(display, XK_F10)) {
                        frame->GetEventHandler()->CallAfter([frame]() {
                            if (!g_IsRecording) frame->StartAction(); else frame->StopAction();
                        });
                    }
                    else if (event.xkey.keycode == XKeysymToKeycode(display, XK_F11)) {
                        frame->GetEventHandler()->CallAfter([frame]() { frame->PauseAction(); });
                    }
                }
            }
            XCloseDisplay(display);
        }, this).detach();
    }

    void OnOpenFolder(wxHyperlinkEvent &event)
    {
        wxLaunchDefaultApplication(m_currentFolderPath);
    }
};

class RecorderApp : public wxApp
{
public:
    virtual bool OnInit()
    {
        RecorderFrame *frame = new RecorderFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(RecorderApp);
