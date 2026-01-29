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
#include <iostream>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

std::atomic<bool> g_IsRecording(false);
std::atomic<bool> g_IsPaused(false);

struct AudioDeviceInfo
{
    ma_device_id id;
    std::string name;
};

void RecordLogic(std::string folderPath, ma_device_id *pSelectedId, bool mute, bool onlyAudio, int width, int height)
{
    if (!wxFileName::Mkdir(folderPath, 0777, wxPATH_MKDIR_FULL))
        return;

    ma_encoder encoder;
    ma_device device;
    bool audioInitialized = false;

    std::string audioPath = folderPath + "/audio.wav";
    std::string videoPath = folderPath + "/video_silent.mp4";
    std::string finalPath = folderPath + (onlyAudio ? "/audio_only.wav" : "/record.mp4");

    if (!mute)
    {
        ma_encoder_config encCfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, 16000);
        if (ma_encoder_init_file(audioPath.c_str(), &encCfg, &encoder) == MA_SUCCESS)
        {
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

            if (ma_device_init(NULL, &devCfg, &device) == MA_SUCCESS)
            {
                ma_device_start(&device);
                audioInitialized = true;
            }
        }
    }

    FILE *pipe = nullptr;
    if (!onlyAudio)
    {
        std::string videoSize = std::to_string(width) + "x" + std::to_string(height);
        std::string cmd = "ffmpeg -y -loglevel quiet -f x11grab -video_size " + videoSize + " -i :0.0 -c:v libx264 -preset ultrafast " + videoPath;
        pipe = popen(cmd.c_str(), "w");
    }

    while (g_IsRecording)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (audioInitialized)
    {
        ma_device_uninit(&device);
        ma_encoder_uninit(&encoder);
    }

    if (pipe)
    {
        fprintf(pipe, "q\n");
        pclose(pipe);
    }

    if (onlyAudio)
    {
        if (audioInitialized) rename(audioPath.c_str(), finalPath.c_str());
    }
    else
    {
        if (audioInitialized && !mute)
        {
            std::string mergeCmd = "ffmpeg -y -loglevel quiet -i " + videoPath + " -i " + audioPath + " -c:v copy -c:a aac -map 0:v:0 -map 1:a:0 " + finalPath;
            system(mergeCmd.c_str());
            remove(videoPath.c_str());
            remove(audioPath.c_str());
        }
        else
        {
            rename(videoPath.c_str(), finalPath.c_str());
        }
    }
}

class RecorderFrame : public wxFrame
{
public:
    RecorderFrame() : wxFrame(NULL, wxID_ANY, "BOBREC PRO", wxDefaultPosition, wxSize(450, 420))
    {
        SetBackgroundColour(wxColour(240, 240, 240));
        m_panel = new wxPanel(this);
        wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

        wxStaticBoxSizer *settingsBox = new wxStaticBoxSizer(wxVERTICAL, m_panel, wxString::FromUTF8("Настройки захвата"));
        
        wxArrayString modes;
        modes.Add(wxString::FromUTF8("Видео + Звук"));
        modes.Add(wxString::FromUTF8("Только Видео"));
        modes.Add(wxString::FromUTF8("Только Звук"));
        m_modeRadio = new wxRadioBox(m_panel, wxID_ANY, wxString::FromUTF8("Режим записи"), wxDefaultPosition, wxDefaultSize, modes, 1, wxRA_SPECIFY_COLS);

        m_deviceChoice = new wxChoice(m_panel, wxID_ANY);
        m_spinDelay = new wxSpinCtrl(m_panel, wxID_ANY, "0");

        settingsBox->Add(m_modeRadio, 0, wxEXPAND | wxALL, 5);
        settingsBox->Add(new wxStaticText(m_panel, wxID_ANY, wxString::FromUTF8("Аудио-устройство:")), 0, wxALL, 5);
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

        wxStaticText *hotkeyHint = new wxStaticText(m_panel, wxID_ANY, wxString::FromUTF8("F10: Старт/Стоп | F11: Пауза"));
        hotkeyHint->SetForegroundColour(wxColour(100, 100, 100));

        m_combinedStatusSizer = new wxBoxSizer(wxHORIZONTAL);
        m_statusTxt = new wxStaticText(m_panel, wxID_ANY, wxString::FromUTF8("Готов"));
        m_statusTxt->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
        m_linkOpenFolder = new wxHyperlinkCtrl(m_panel, wxID_ANY, wxString::FromUTF8("(открыть папку)"), "");
        m_linkOpenFolder->Hide();

        m_combinedStatusSizer->Add(m_statusTxt, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
        m_combinedStatusSizer->Add(m_linkOpenFolder, 0, wxALIGN_CENTER_VERTICAL);

        mainSizer->Add(settingsBox, 0, wxEXPAND | wxALL, 15);
        mainSizer->Add(btnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        mainSizer->Add(hotkeyHint, 0, wxALIGN_CENTER | wxTOP, 5);
        mainSizer->Add(m_combinedStatusSizer, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 15);

        m_btnPause->Enable(false);
        m_btnStop->Enable(false);
        m_panel->SetSizer(mainSizer);

        m_btnStart->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { StartAction(); });
        m_btnPause->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { PauseAction(); });
        m_btnStop->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { StopAction(); });
        m_modeRadio->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent &) { 
            m_deviceChoice->Enable(m_modeRadio->GetSelection() != 1); 
        });

        m_countdownTimer = new wxTimer(this);
        Bind(wxEVT_TIMER, &RecorderFrame::OnTimerTick, this, m_countdownTimer->GetId());

        RefreshDevices();
        StartGlobalHotkeyThread();
        Centre();
    }

private:
    wxPanel *m_panel;
    wxButton *m_btnStart, *m_btnPause, *m_btnStop;
    wxSpinCtrl *m_spinDelay;
    wxChoice *m_deviceChoice;
    wxRadioBox *m_modeRadio;
    wxStaticText *m_statusTxt;
    wxHyperlinkCtrl *m_linkOpenFolder;
    wxBoxSizer *m_combinedStatusSizer;
    wxString m_currentFolderPath;
    std::vector<AudioDeviceInfo> m_devices;
    wxTimer *m_countdownTimer;
    int m_remainingSeconds;

    void RefreshDevices()
    {
        m_devices.clear();
        m_deviceChoice->Clear();
        ma_context context;
        if (ma_context_init(NULL, 0, NULL, &context) == MA_SUCCESS) {
            ma_device_info *pCaptureInfos; ma_uint32 captureCount;
            if (ma_context_get_devices(&context, NULL, NULL, &pCaptureInfos, &captureCount) == MA_SUCCESS) {
                for (ma_uint32 i = 0; i < captureCount; ++i) {
                    AudioDeviceInfo info = {pCaptureInfos[i].id, pCaptureInfos[i].name};
                    m_devices.push_back(info);
                    
                    wxString nameLower = wxString::FromUTF8(info.name.c_str()).Lower();
                    wxString prefix;

                    // Более надежная проверка на системный звук (Monitor или Output)
                    if (nameLower.Contains("monitor") || nameLower.Contains("output")) {
                        prefix = wxString::FromUTF8("[СИСТЕМА] ");
                    } else {
                        prefix = wxString::FromUTF8("[МИКРОФОН] ");
                    }
                    
                    m_deviceChoice->Append(prefix + wxString::FromUTF8(info.name.c_str()));
                }
            }
            ma_context_uninit(&context);
        }
        if (m_deviceChoice->GetCount() > 0) m_deviceChoice->SetSelection(0);
    }

    void OnTimerTick(wxTimerEvent&) {
        if (m_remainingSeconds > 0) {
            m_statusTxt->SetLabel(wxString::Format(wxString::FromUTF8("Старт через: %d..."), m_remainingSeconds--));
        } else {
            m_countdownTimer->Stop();
            wxBell();
            ExecuteStart();
        }
    }

    void StartAction() {
        if (g_IsRecording || m_countdownTimer->IsRunning()) return;
        m_remainingSeconds = m_spinDelay->GetValue();
        m_linkOpenFolder->Hide();
        if (m_remainingSeconds > 0) {
            m_btnStart->Enable(false); m_btnStop->Enable(true);
            m_statusTxt->SetLabel(wxString::Format(wxString::FromUTF8("Старт через: %d..."), m_remainingSeconds--));
            m_countdownTimer->Start(1000);
        } else {
            ExecuteStart();
        }
    }

    void ExecuteStart() {
        g_IsRecording = true; g_IsPaused = false;
        m_btnStart->Enable(false); m_btnPause->Enable(true); m_btnStop->Enable(true);
        m_statusTxt->SetLabel(wxString::FromUTF8("• ЗАПИСЬ..."));
        m_statusTxt->SetForegroundColour(wxColour(200, 0, 0));

        wxDateTime now = wxDateTime::Now();
        m_currentFolderPath = "records/" + now.FormatISODate() + "/" + now.Format("%H-%M-%S");

        int mode = m_modeRadio->GetSelection();
        bool onlyAudio = (mode == 2);
        bool mute = (mode == 1);
        
        int devIdx = m_deviceChoice->GetSelection();
        ma_device_id* pId = (devIdx != wxNOT_FOUND) ? &m_devices[devIdx].id : nullptr;

        std::thread(RecordLogic, m_currentFolderPath.ToStdString(), pId, mute, onlyAudio, 
                    wxGetDisplaySize().x, wxGetDisplaySize().y).detach();
        m_panel->Layout();
    }

    void PauseAction() {
        if (!g_IsRecording) return;
        g_IsPaused = !g_IsPaused;
        m_btnPause->SetLabel(g_IsPaused ? wxString::FromUTF8("▶ ПРОДОЛЖИТЬ") : wxString::FromUTF8("⏸ ПАУЗА"));
    }

    void StopAction() {
        if (m_countdownTimer->IsRunning()) {
            m_countdownTimer->Stop();
            m_statusTxt->SetLabel(wxString::FromUTF8("Отменено"));
            m_statusTxt->SetForegroundColour(wxColour(0, 0, 0));
        } else if (g_IsRecording) {
            g_IsRecording = false;
            m_statusTxt->SetLabel(wxString::FromUTF8("Сохранено"));
            m_statusTxt->SetForegroundColour(wxColour(0, 150, 0));
            m_linkOpenFolder->SetURL("file://" + wxFileName(m_currentFolderPath).GetAbsolutePath());
            m_linkOpenFolder->Show();
        }
        m_btnStart->Enable(true); m_btnPause->Enable(false); m_btnStop->Enable(false);
        m_btnPause->SetLabel(wxString::FromUTF8("⏸ ПАУЗА"));
        m_panel->Layout();
    }

    void OnOpenFolder(wxHyperlinkEvent&) {
        wxLaunchDefaultApplication(wxFileName(m_currentFolderPath).GetAbsolutePath());
    }

    void StartGlobalHotkeyThread() {
        std::thread([this]() {
            Display* dpy = XOpenDisplay(NULL);
            if (!dpy) return;
            int kF10 = XKeysymToKeycode(dpy, XK_F10), kF11 = XKeysymToKeycode(dpy, XK_F11);
            XGrabKey(dpy, kF10, AnyModifier, DefaultRootWindow(dpy), True, GrabModeAsync, GrabModeAsync);
            XGrabKey(dpy, kF11, AnyModifier, DefaultRootWindow(dpy), True, GrabModeAsync, GrabModeAsync);
            XEvent ev;
            while (true) {
                XNextEvent(dpy, &ev);
                if (ev.type == KeyPress) {
                    if (ev.xkey.keycode == kF10) { if(!g_IsRecording && !m_countdownTimer->IsRunning()) StartAction(); else StopAction(); }
                    else if (ev.xkey.keycode == kF11) PauseAction();
                }
            }
        }).detach();
    }
};

class RecorderApp : public wxApp {
public:
    virtual bool OnInit() { (new RecorderFrame())->Show(); return true; }
};
wxIMPLEMENT_APP(RecorderApp);
