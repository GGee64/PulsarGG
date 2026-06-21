//PulsarGG v1.0.1 (c) GGee64

#define INITGUID
#include <windows.h>
#include <d2d1.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <shellapi.h>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <mmsystem.h>
#include <cstring>
#include <cstdint>

#ifndef AUDCLNT_BUFFERFLAGS_SILENT
#define AUDCLNT_BUFFERFLAGS_SILENT 0x8
#endif

#define NOMINMAX
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shlwapi.lib")

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID   IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID   IID_IAudioClient = __uuidof(IAudioClient);
const IID   IID_IAudioRenderClient = __uuidof(IAudioRenderClient);
const IID   IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);
const IID   IID_IMMNotificationClient = __uuidof(IMMNotificationClient);
//const IID   IID_IAudioSessionManager2 = __uuidof(IAudioSessionManager2);
//const CLSID CLSID_IAudioSessionEnumerator = __uuidof(IAudioSessionEnumerator);

HWND hMainWnd = nullptr;
HWND hOverlayWnd = nullptr;
NOTIFYICONDATA nid = {};
std::atomic<bool> g_running = true;
int screenWidth = 0, screenHeight = 0;
HICON hTrayIcon = nullptr;
std::thread* pAnimThread = nullptr;
std::condition_variable g_cv;
std::mutex g_cv_m;
std::atomic<bool> g_deviceChanged{ false };
std::atomic<bool> g_streamInvalidated{ false }; // Для AUDCLNT_E_DEVICE_INVALIDATED

struct OverlayState {
    ID2D1Factory* factory = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brushes[64] = {};
    bool ready = false;
};

struct Band {
    float value = 0.0f;
    void update(float target) {
        if (target < 0.0f) target = 0.0f;
        if (target > 1.0f) target = 1.0f;
        if (target > value) value += (target - value) * 0.50f;
        else {
            float decay = (value * 0.01f > 0.05f) ? (value * 0.01f) : 0.01f;
            value -= decay;
        }
    }
};

class AudioDeviceNotifier : public IMMNotificationClient {
    long m_cRef = 0;
    std::atomic<bool>* pChangedFlag = nullptr;

public:
    explicit AudioDeviceNotifier(std::atomic<bool>* flag) : pChangedFlag(flag) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_cRef); }

    ULONG STDMETHODCALLTYPE Release() override {
        long ref = InterlockedDecrement(&m_cRef);
        if (ref == 0) delete this;
        return ref;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        // Используем __uuidof для избежания ошибок линкера
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppvObject = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    // ✅ Эти методы ОБЯЗАНЫ быть реализованы!
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) override { return S_OK; }

    // ⚠️ ЭТОТ МЕТОД ИСПРАВЛЯЕТ ОШИБКУ "АБСТРАКТНОГО КЛАССА"
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR /*pwstrDeviceId*/, DWORD /*dwNewState*/) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) override { return S_OK; }

    // Этот метод сработает при переключении устройства в настройках Windows
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR /*pwstrDeviceId*/) override {
        if (flow != eRender || role != eConsole) return S_OK;

        OutputDebugStringW(L"[Audio] Default device changed! Triggering hot-swap.\n");
        if (pChangedFlag) pChangedFlag->store(true);
        g_cv.notify_all();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/) override { return S_OK; }
};

class AudioCapture {
    const int numBands = 64;
    int drainFrames = 0; // ✅ ИСПРАВЛЕНО: поле объявлено и доступно
    bool streamValid = false;

public:
    std::wstring lastDeviceId;
    mutable std::mutex id_mtx;
    IAudioClient* pClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;
    WAVEFORMATEX* pwfx = nullptr;
    mutable std::mutex mtx;
    std::atomic<bool> streamInvalidated{ false };

private:
    Band bands[64] = {};
    bool initialized = false;

    void LogAudioPipelineStatus(LPCWSTR deviceId, WAVEFORMATEX* pwfx) {
        if (!pwfx) return;

        bool isEncoded = false;
        LPCWSTR encoderName = L"Unknown";

        // 1. Проверяем расширенный формат (EXTENSIBLE), где хранится реальный SubFormat
        if (pwfx->wFormatTag == 0xFFFE && pwfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);

            // Определяем GUID вручную, исключая зависимости от заголовков SDK
            static const GUID guidDolbyAC3 = { 0xe4ac3e25, 0x2b87, 0x11d3, {0xa6, 0xb3, 0x00, 0xc0, 0x4f, 0x79, 0x0c, 0xd0} };
            static const GUID guidMPEGADTS = { 0x00000016, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71} };

            if (memcmp(&ext->SubFormat, &guidDolbyAC3, sizeof(GUID)) == 0) {
                isEncoded = true; encoderName = L"Dolby AC3/SPDIF";
            }
            else if (memcmp(&ext->SubFormat, &guidMPEGADTS, sizeof(GUID)) == 0) {
                isEncoded = true; encoderName = L"MPEG Audio (ADTS)";
            }
        }
        // 2. Fallback для не-EXTENSIBLE форматов
        else if (pwfx->wFormatTag == 0x2000) { isEncoded = true; encoderName = L"DTS"; }
        else if (pwfx->wFormatTag == 0x0050) { isEncoded = true; encoderName = L"Windows Media Audio"; }

        OutputDebugStringW(L"[Pipeline] Device: ");
        OutputDebugStringW(deviceId ? deviceId : L"N/A");

        char msg[256];
        sprintf_s(msg, " Tag=0x%04X Fmt=%s Ch=%d Rate=%d [LOOPBACK %s]\n",
            pwfx->wFormatTag, isEncoded ? "ENCODED (Protected)" : "PCM (Clean)",
            pwfx->nChannels, pwfx->nSamplesPerSec, isEncoded ? "BLOCKED" : "WORKS");
        OutputDebugStringA(msg);
    }

    void log(const char* fmt, ...) {
        char buf[256]; va_list args; va_start(args, fmt); vsnprintf_s(buf, sizeof(buf), fmt, args); va_end(args);
        OutputDebugStringA("[Audio]"); OutputDebugStringA(buf);
    }

    void log_hr(const char* prefix, HRESULT hr) {
        char buf[64]; sprintf_s(buf, "[%s: 0x%08X]\n", prefix, hr);
        OutputDebugStringA(buf);
    }

    void CleanupCOM() {
        if (pClient) { pClient->Release(); pClient = nullptr; }
        if (pCaptureClient) { pCaptureClient->Release(); pCaptureClient = nullptr; }
        if (pwfx) { CoTaskMemFree(pwfx); pwfx = nullptr; }
    }

public:
    bool StartWithRoutingWait() {
        if (!Start()) return false;

        const auto startTime = std::chrono::steady_clock::now();
        constexpr std::chrono::milliseconds maxWait(2000);

        while (std::chrono::steady_clock::now() - startTime < maxWait) {
            BYTE* pData = nullptr; UINT32 numFrames = 0; DWORD flags = 0;
            HRESULT hr = pCaptureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);

            if (SUCCEEDED(hr) && numFrames > 0 && pData != nullptr && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                float energy = 0.0f;
                int ch = (pwfx->nChannels > 0) ? pwfx->nChannels : 2;

                if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                    const float* f = reinterpret_cast<const float*>(pData);
                    for (UINT32 i = 0; i < numFrames; ++i) energy += fabsf(f[i * ch]);
                }
                else if (pwfx->wBitsPerSample == 16) {
                    const int16_t* s = reinterpret_cast<const int16_t*>(pData);
                    for (UINT32 i = 0; i < numFrames; ++i) energy += abs(s[i * ch]);
                }

                pCaptureClient->ReleaseBuffer(numFrames);

                // Если энергия пакета > порога аппаратного шума → Windows перенаправил сессии. Активация успешна.
                if (energy > 5.0f) return true;
            }
            else {
                if (pData && numFrames > 0) pCaptureClient->ReleaseBuffer(numFrames);
            }

            if (!initialized || !pCaptureClient) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        OutputDebugStringA("[Audio] Routing timeout exceeded. Stream inactive.\n");
        return false;
    }

    bool Start() {
        initialized = false;
        drainFrames = 3;
        streamInvalidated.store(false);

        if (pClient) pClient->Release(); pClient = nullptr;
        if (pCaptureClient) pCaptureClient->Release(); pCaptureClient = nullptr;
        if (pwfx) { CoTaskMemFree(pwfx); pwfx = nullptr; }

        IMMDeviceEnumerator* pEnumerator = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnumerator);
        if (!SUCCEEDED(hr) || !pEnumerator) { log("[Init] Enumerator failed.\n"); return false; }

        IMMDevice* pEndpoint = nullptr;
        const ERole roles[] = { eConsole, eMultimedia };
        for (auto role : roles) {
            hr = pEnumerator->GetDefaultAudioEndpoint(eRender, role, &pEndpoint);
            if (SUCCEEDED(hr) && pEndpoint) break;
        }
        pEnumerator->Release();

        if (!SUCCEEDED(hr) || !pEndpoint) { log("[Init] No endpoint found.\n"); return false; }

        LPWSTR fullId = nullptr;
        std::wstring cleanId = L"";
        if (SUCCEEDED(pEndpoint->GetId(&fullId))) {
            size_t pos = std::wstring(fullId).find_first_of(L'{');
            if (pos != std::string::npos) cleanId = std::wstring(fullId).substr(pos, 38);
            CoTaskMemFree(fullId);
        }

        {
            std::lock_guard<std::mutex> lock(id_mtx);
            lastDeviceId = cleanId.empty() ? L"UNKNOWN" : cleanId;
        }

        hr = pEndpoint->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pClient);
        log_hr("[Init] Activate", hr);
        pEndpoint->Release();

        if (!SUCCEEDED(hr) || !pClient) { log("[Init] Activate failed.\n"); return false; }

        WAVEFORMATEX* pwfxTemp = nullptr;
        hr = pClient->GetMixFormat(&pwfxTemp);
        if (FAILED(hr) || !pwfxTemp) {
            log_hr("[Init] GetMixFormat", hr);
            pClient->Release(); pClient = nullptr; return false;
        }

        LONGLONG hnsPeriod = 0;
        pClient->GetDevicePeriod(&hnsPeriod, NULL);
        LONGLONG hnsBufferDuration = (SUCCEEDED(hr) && hnsPeriod > 0) ? (hnsPeriod * 2) : 300000;

        char fmtLog[120]; sprintf_s(fmtLog, "[Init] Period=%lld Buffer=%lld Tag=0x%04X Rate=%d Bits=%d Ch=%d\n",
            hnsPeriod, hnsBufferDuration, pwfxTemp->wFormatTag, pwfxTemp->nSamplesPerSec, pwfxTemp->wBitsPerSample, pwfxTemp->nChannels);
        OutputDebugStringA(fmtLog);

        hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, hnsBufferDuration, 0, pwfxTemp, NULL);
        if (FAILED(hr)) {
            char failLog[80]; sprintf_s(failLog, "[Init] Initialize FAILED: 0x%08X\n", hr); OutputDebugStringA(failLog);
            CoTaskMemFree(pwfxTemp); pClient->Release(); pClient = nullptr; return false;
        }

        pwfx = pwfxTemp;
        hr = pClient->GetService(IID_IAudioCaptureClient, (void**)&pCaptureClient);
        if (FAILED(hr) || !pCaptureClient) {
            log_hr("[Init] GetService", hr);
            pClient->Release(); pClient = nullptr; return false;
        }

        hr = pClient->Start();
        log_hr("[Init] Start", hr);

        if (SUCCEEDED(hr)) {
            initialized = true;
            log("LOOPBACK ACTIVE!\n");
            return true;
        }
        else { CleanupCOM(); return false; }
    }

    void Stop() {
        if (!initialized) return;
        try {
            if (pClient) pClient->Stop();
            CleanupCOM();
        }
        catch (...) {}
        initialized = false;
        streamValid = false;
        drainFrames = 0;
    }

    void Update() {
        // Критичная проверка: если поток не активен или инвалидирован — игнорируем вызовы из анимационного потока
        if (!initialized || !pCaptureClient) return;

        std::lock_guard<std::mutex> lock(mtx);
        float targets[64] = {};

        DWORD flags = 0; BYTE* pData = nullptr; UINT32 numFrames = 0;
        HRESULT hr = pCaptureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);

        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            streamInvalidated.store(true);
            this->Stop();
            return;
        }

        // Ошибка захвата (не инвалидация) — не вызываем ReleaseBuffer! Ждём следующего кадра от драйвера.
        if (FAILED(hr)) return;

        // Очередь пуста или нет данных → корректно разблокируем пакет и гасим полосу через decay
        if (numFrames == 0 || !pData) {
            pCaptureClient->ReleaseBuffer(0);
            for (int i = 0; i < numBands; ++i) bands[i].update(0.0f);
            return;
        }

        // Shared Loopback ВСЕГДА стерео-дамник (2 канала). Не доверяем метаданным устройства.
        const int ch = 2;
        bool isFloat = false;

        if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            isFloat = true;
        }
        else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
            static const unsigned char pcmSig[] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
            if (memcmp(&ext->SubFormat, pcmSig, 16) == 0) isFloat = false;
        }
        // Fallback для DRM/SPDIF/зашифрованных выходов: считаем как float (безопаснее для loopback)
        if (!isFloat && pwfx->wFormatTag != WAVE_FORMAT_PCM) isFloat = true;

        // ✅ КЛЮЧЕВОЕ ИСПРАВЛЕНИЕ: Нормализация по пику кадра (обходит системную громкость на любом выводе)
        double maxInFrame = 0.0;
        if (isFloat || pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            const float* fSamp = reinterpret_cast<const float*>(pData);
            for (UINT32 i = 0; i < numFrames; ++i) {
                double s = fabsf(fSamp[i * ch + 0]); double r = fabsf(fSamp[i * ch + 1]);
                if (s > maxInFrame) maxInFrame = s;
                if (r > maxInFrame) maxInFrame = r;
            }
        }
        else {
            const int16_t* iSamp = reinterpret_cast<const int16_t*>(pData);
            for (UINT32 i = 0; i < numFrames; ++i) {
                double s = static_cast<double>(abs(iSamp[i * ch + 0])) / 32768.0;
                double r = static_cast<double>(abs(iSamp[i * ch + 1])) / 32768.0;
                if (s > maxInFrame) maxInFrame = s;
                if (r > maxInFrame) maxInFrame = r;
            }
        }

        // Ниже порога аппаратного шума → это тишина. Масштаб = 0 (убивает ложный пульс на 0% и дребезг волн)
        const double SILENCE_FLOOR = 0.002;
        float frameScale = (maxInFrame < SILENCE_FLOOR) ? 0.0f : (0.45f / static_cast<float>(maxInFrame));

        // ВАЖНО: ReleaseBuffer вызывается ровно один раз после GetBuffer (по spec WASAPI)
        pCaptureClient->ReleaseBuffer(numFrames);

        double totalEnergy = 0.0;
        float step = static_cast<float>(numFrames) / numBands;
        const void* samples = pData;

        for (int band = 0; band < numBands; ++band) {
            int startIdx = static_cast<int>(floorf(band * step));
            int endIdx = static_cast<int>(ceilf((band + 1) * step));
            if (endIdx > numFrames) endIdx = numFrames;

            double sumSq = 0.0; int count = 0;

            if (isFloat || pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                const float* fSamp = reinterpret_cast<const float*>(samples);
                for (int i = startIdx; i < endIdx; ++i, ++count) {
                    double sL = fabsf(fSamp[i * ch + 0]); double sR = fabsf(fSamp[i * ch + 1]);
                    sumSq += sL * sL + sR * sR;
                }
            }
            else {
                const int16_t* iSamp = reinterpret_cast<const int16_t*>(samples);
                for (int k = startIdx; k < endIdx; ++k, ++count) {
                    float sL = static_cast<float>(abs(iSamp[k * ch + 0])) / 32768.0f;
                    float sR = static_cast<float>(abs(iSamp[k * ch + 1])) / 32768.0f;
                    sumSq += sL * sL + sR * sR;
                }
            }

            double rms = count > 0 ? sqrt(sumSq / count) : 0.0;
            // Масштабируем RMS относительно пика кадра, а не абсолютного значения
            float bandVal = static_cast<float>(rms * frameScale);

            targets[band] = (bandVal < 0.0f) ? 0.0f : ((bandVal > 1.0f) ? 1.0f : bandVal);
            totalEnergy += targets[band];
        }

        // ТВОЯ ЛОГИКА ОБНОВЛЕНИЯ БЕЗ ИЗМЕНЕНИЙ
        for (int i = 0; i < numBands; ++i) bands[i].update(targets[i]);
    }


    Band GetBand(int i) const {
        if (i < 0 || i >= numBands) return Band();
        std::lock_guard<std::mutex> lock(mtx); return bands[i];
    }
};

AudioCapture g_audio;

void GetScreenSize() { screenWidth = GetSystemMetrics(SM_CXSCREEN); screenHeight = GetSystemMetrics(SM_CYSCREEN); }

void DrawEqualizer(ID2D1HwndRenderTarget* rt, const OverlayState* state) {
    if (!rt || !state->ready) return;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f));

    Band data[64]; for (int e = 0; e < 64; ++e) data[e] = g_audio.GetBand(e);
    const int NUM_BANDS = 64;

    float gapY = fmax(1.0f, screenHeight * 0.003f);
    float barH = (screenHeight - gapY * (NUM_BANDS - 1)) / static_cast<float>(NUM_BANDS);
    if (barH < 2.0f) barH = 2.0f;

    float maxGrowW = screenWidth * 0.03f;
    float marginX = fmax(0.0f, screenWidth * 0.0f);

    static auto lastTick = std::chrono::steady_clock::now();
    static double waveT = 0.0;
    static float waveState = 0.0f;

    float dt = static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now() - lastTick).count());
    lastTick = std::chrono::steady_clock::now();
    waveT += dt * 1.5f;

    // ✅ ИСПРАВЛЕНИЕ: Широкий гистерезис + плавное сглаживание убирают «дребезг» волн на 0-10% громкости
    float totalEnergy = 0.0f; for (int e = 0; e < NUM_BANDS; ++e) totalEnergy += data[e].value;
    float avgEnergy = totalEnergy / static_cast<float>(NUM_BANDS);

    // Медленное экспоненциальное сглаживание (коэффициент 0.92 гасит микрофлуктуации AGC и шум DPC-петли колонок)
    static float smoothEnergy = 0.0f;
    smoothEnergy = smoothEnergy * 0.92f + avgEnergy * 0.08f;

    // ✅ ПЛАВНАЯ ВОЛНА: ровный переход за 0.5с в любую сторону, без инерции на просадках FPS
    const float WAVE_OFF_THRESH = 0.12f;
    const float WAVE_ON_THRESH = 0.04f;
    const float TRANSITION_TIME = 0.5f; // Длительность плавного хода в секундах

    static float targetWave = 0.0f;

    if (smoothEnergy > WAVE_OFF_THRESH) targetWave = 0.0f;       // Музыка → целимся вниз
    else if (smoothEnergy < WAVE_ON_THRESH) targetWave = 1.0f;   // Тишина → целимся вверх

    float diff = targetWave - waveState;
    float maxStep = dt / TRANSITION_TIME;
    if (maxStep > 1.0f) maxStep = 1.0f; // Защита от просадок FPS: не телепортируется при лагах окна

    waveState += diff * ((fabsf(diff) <= maxStep) ? 1.0f : maxStep);
    if (waveState < 0.0f) waveState = 0.0f;
    if (waveState > 1.0f) waveState = 1.0f;

    float waveP[64];
    for (int i = 0; i < NUM_BANDS; ++i) {
        waveP[i] = (sin(waveT * 0.9f + i * 0.2f) * 0.3f + sin(waveT * 1.7f - i * 0.4f) * 0.15f + 0.45f);
    }

    float waveAmp = maxGrowW * 0.28f; // Базовая амплитуда волн в тишине

    // === ЛЕВАЯ СТОРОНА (безопасное переключение без схлопывания ширины) ===
    for (int i = 0; i < NUM_BANDS; ++i) {
        if (!state->brushes[i]) continue;
        float audioW = data[i].value * maxGrowW;
        float waveW = waveP[i] * waveAmp * waveState;

        // Если тишина (waveState ≈ 1) → рисуем волны. Иначе строго по пульсу.
        // Никогда не умножаем на waveState при музыке, иначе ширина уйдёт в 0.
        float w = (smoothEnergy < WAVE_ON_THRESH) ? waveW : audioW;
        if (w < 2.0f) w = 2.0f;
        rt->FillRectangle(D2D1::RectF(marginX, i * (barH + gapY), marginX + w, i * (barH + gapY) + barH), state->brushes[i]);
    }

    // === ПРАВАЯ СТОРОНА (зеркально по частотам/цветам) ===
    for (int i = 0; i < NUM_BANDS; ++i) {
        int bIdx = NUM_BANDS - 1 - i;
        if (!state->brushes[bIdx]) continue;

        float audioW = data[bIdx].value * maxGrowW;
        float waveW = waveP[i] * waveAmp * waveState;

        float w = (smoothEnergy < WAVE_ON_THRESH) ? waveW : audioW;
        if (w < 2.0f) w = 2.0f;
        rt->FillRectangle(D2D1::RectF(screenWidth - marginX - w, i * (barH + gapY), screenWidth - marginX, i * (barH + gapY) + barH), state->brushes[bIdx]);
    }

    HRESULT hr = rt->EndDraw();
    if (FAILED(hr)) return;
}


LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static OverlayState* state = nullptr;
    switch (msg) {
    case WM_CREATE: {
        state = new OverlayState(); ZeroMemory(state, sizeof(OverlayState));
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &state->factory))) return -1;

        RECT rc = { 0 }; GetWindowRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        if (FAILED(state->factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(w, h)), &state->rt)) || !state->rt) {
            MessageBox(NULL, L"D2D Init Fail", L"Error", MB_ICONERROR); return -1;
        }

        const int NUM_BANDS = 64;
        for (int i = 0; i < NUM_BANDS; ++i) {
            float t = static_cast<float>(i) / (NUM_BANDS - 1);
            D2D1::ColorF c(0.4f + t * 0.6f, 0.9f - t * 0.3f, 1.0f, 1.0f);
            state->rt->CreateSolidColorBrush(c, &state->brushes[i]);
        }
        state->ready = true;
        SetPropW(hwnd, L"OverlayState", reinterpret_cast<HANDLE>(state));
        SetTimer(hwnd, 1, 1000 / 75, NULL);
        return 0;
    }
    case WM_TIMER: {
        OverlayState* s = reinterpret_cast<OverlayState*>(GetPropW(hwnd, L"OverlayState"));
        if (s && s->ready && s->rt) DrawEqualizer(s->rt, s);
        return 0;
    }
    case WM_SIZE: {
        OverlayState* s = reinterpret_cast<OverlayState*>(GetPropW(hwnd, L"OverlayState"));
        if (!s || !s->rt) return 0;
        RECT rc = { 0 }; GetWindowRect(hwnd, &rc);
        s->rt->Resize(D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_DESTROY: {
        OverlayState* s = reinterpret_cast<OverlayState*>(GetPropW(hwnd, L"OverlayState"));
        RemovePropW(hwnd, L"OverlayState");
        if (s) {
            for (int i = 0; i < 64; ++i) if (s->brushes[i]) s->brushes[i]->Release();
            if (s->rt) s->rt->Release();
            if (s->factory) s->factory->Release();
            delete s;
        }
        return 0;
    }
    case WM_NCHITTEST: return HTTRANSPARENT;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ✅ Вспомогательная функция ожидания данных от нового потока (для AnimationThread)
bool WaitForValidAudio(AudioCapture* audio, std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - start < timeout) {
        if (!audio->pClient || !audio->pCaptureClient) return false;

        // Используем RAII для мьютекса, чтобы избежать ошибок unlock
        std::lock_guard<std::mutex> lock(audio->mtx);

        DWORD flags = 0; BYTE* pData = nullptr; UINT32 numFrames = 0;
        HRESULT hr = audio->pCaptureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);

        if (SUCCEEDED(hr) && numFrames > 0 && !((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)) {
            // Если флаг тишины НЕ стоит и есть данные, считаем звук
            float energy = 0.0f; int ch = audio->pwfx->nChannels;

            if (audio->pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                const float* f = reinterpret_cast<const float*>(pData);
                for (UINT32 i = 0; i < numFrames; ++i) energy += fabsf(f[i * ch]);
            }
            else if (audio->pwfx->wBitsPerSample == 16) {
                const int16_t* s = reinterpret_cast<const int16_t*>(pData);
                for (UINT32 i = 0; i < numFrames; ++i) energy += abs(s[i * ch]);
            }

            // Освобождаем пакет внутри лок-гарда, это безопасно (буфер передан в систему)
            audio->pCaptureClient->ReleaseBuffer(numFrames);

            if (energy > 0.001f) return true;
        }
        else {
            // Если буфер пуст или тих - просто игнорируем и ждем
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // Не блокируем CPU
    }
    return false;
}

void AnimationThread() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    g_audio.Start();

    auto lastTime = std::chrono::steady_clock::now();
    constexpr double targetDelta = 1000.0 / 75.0;
    double accumulator = 0.0f;

    while (g_running) {
        bool needDeviceSwitch = g_deviceChanged.exchange(false);

        if (needDeviceSwitch || g_audio.streamInvalidated.exchange(false)) {
            OutputDebugStringA("[Audio] Device switched / stream invalidated. Hot-reloading...\n");

            // 1. Полная остановка и сброс состояния COM
            g_audio.Stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Даем драйверу освободить аппаратные ресурсы

            // 2. Безопасный рестрим с гарантией активации
            if (g_audio.Start()) {
                OutputDebugStringA("[Audio] Stream restarted successfully.\n");
            }
            else {
                // Fallback если первый старт провалился (возможно, APM еще не маршрутизировал)
                OutputDebugStringA("[Audio] First start failed. Retrying once...\n");
                Sleep(100);
                g_audio.Start();
            }
        }

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float, std::milli>(now - lastTime).count();
        lastTime = now;
        accumulator += dt;

        while (accumulator >= targetDelta) {
            g_audio.Update();
            if (hOverlayWnd && IsWindowVisible(hOverlayWnd)) InvalidateRect(hOverlayWnd, nullptr, FALSE);
            accumulator -= targetDelta;
        }

        std::unique_lock<std::mutex> lk(g_cv_m);
        g_cv.wait_for(lk, std::chrono::milliseconds(50), [] { return !g_running; });
    }

    g_audio.Stop();
    CoUninitialize();
}


void ShowTrayMenu() {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 2, L"Exit");
    POINT pt; GetCursorPos(&pt); SetForegroundWindow(hMainWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hMainWnd, NULL); DestroyMenu(hMenu);
}

void CleanupAndExit(HWND hwnd) {
    g_running = false;
    g_cv.notify_all();

    if (nid.hIcon) DestroyIcon(nid.hIcon); nid.hIcon = nullptr;
    Shell_NotifyIcon(NIM_DELETE, &nid);
    if (hOverlayWnd) { DestroyWindow(hOverlayWnd); hOverlayWnd = nullptr; }
    if (pAnimThread && pAnimThread->joinable()) {
        pAnimThread->join(); delete pAnimThread; pAnimThread = nullptr;
    }
    PostQuitMessage(0); DestroyWindow(hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        ZeroMemory(&nid, sizeof(NOTIFYICONDATA));
        nid.cbSize = sizeof(NOTIFYICONDATA); nid.hWnd = hMainWnd = hwnd; nid.uID = 1; nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_USER + 1; nid.uVersion = NOTIFYICON_VERSION_4;

        HICON hTempIcon = (HICON)LoadImageW(GetModuleHandle(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        if (!hTempIcon || hTempIcon == (HICON)-1) { OutputDebugStringW(L"[Tray] Equalizer.ico not found. Using fallback.\n"); hTempIcon = LoadIconW(NULL, IDI_APPLICATION); }
        nid.hIcon = hTempIcon; hTrayIcon = hTempIcon; wcscpy_s(nid.szTip, _countof(nid.szTip), L"Pulsar GG");

        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
        if (!ok) { ShowWindow(hwnd, SW_SHOWNORMAL); Sleep(100); Shell_NotifyIconW(NIM_MODIFY, &nid); ShowWindow(hwnd, SW_HIDE); }
        OutputDebugString(ok ? L"[Tray] Icon registered.\n" : L"[Tray] FAILED.\n");
        return 0;
    }
    case WM_USER + 1: {
        if (lParam == WM_RBUTTONDOWN) ShowTrayMenu();
        else if (lParam == WM_LBUTTONDOWN) { BOOL vis = IsWindowVisible(hOverlayWnd); ShowWindow(hOverlayWnd, vis ? SW_HIDE : SW_SHOWNA); }
        return 0;
    }
    case WM_COMMAND: if (LOWORD(wParam) == 2 || LOWORD(wParam) == SC_CLOSE) CleanupAndExit(hwnd); return 0;
    case WM_CLOSE: CleanupAndExit(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // ✅ STA для COM-уведомлений IMMDeviceEnumerator
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetScreenSize();

    WNDCLASS wc = { 0 }; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"PulseTrayWindow"; RegisterClass(&wc);
    hMainWnd = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PulseTrayWindow", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!hMainWnd) return -1;

    WNDCLASS ov = { 0 }; ov.lpfnWndProc = OverlayWndProc; ov.hInstance = hInst; ov.lpszClassName = L"PulseOverlay";
    ov.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH); ov.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClass(&ov);

    hOverlayWnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        L"PulseOverlay", L"", WS_POPUP, 0, 0, screenWidth, screenHeight, NULL, NULL, hInst, NULL);
    if (!hOverlayWnd) return -1;

    SetPropW(hOverlayWnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));

    ShowWindow(hOverlayWnd, SW_SHOWNA);
    SetLayeredWindowAttributes(hOverlayWnd, RGB(0, 0, 0), 255, LWA_COLORKEY);

    // ✅ РЕГИСТРАЦИЯ ОБРАТНОГО ВЫЗОВА СМЕНЫ УСТРОЙСТВ
    AudioDeviceNotifier* pNotifier = new AudioDeviceNotifier(&g_deviceChanged);
    IMMDeviceEnumerator* pEnum = nullptr;

    if (SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnum))) {
        // Регистрация выполняется в STA-потоке (WinMain), что корректно для COM
        pEnum->RegisterEndpointNotificationCallback(pNotifier);
        pEnum->Release();
    }
    else {
        delete pNotifier;
        OutputDebugStringA("[Init] IMMDeviceEnumerator registration failed.\n");
    }

    pAnimThread = new std::thread(AnimationThread);

    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

    // ✅ ОЧИСТКА COM УВЕДОМЛЕНИЙ
    if (pEnum) pEnum->UnregisterEndpointNotificationCallback(pNotifier);
    delete pNotifier;

    CleanupAndExit(hMainWnd); return 0;
}
