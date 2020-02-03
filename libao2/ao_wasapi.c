#define	COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <SDKDDKVer.h>
#include <Windows.h>
#include <initguid.h>
#include <Audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <immintrin.h>

#include "config.h"
#include "libaf/af_format.h"
#include "audio_out.h"
#include "audio_out_internal.h"
#include "mp_msg.h"
#include "libvo/fastmemcpy.h"
#include "osdep/timer.h"
#include "subopt-helper.h"

static const ao_info_t info =
{
	"Windows Audio Session API",
	"wasapi",
	"h.wang@kimoto.com.cn",
	""
};

LIBAO_EXTERN(wasapi)

static IAudioClient*pAudioClient = NULL;
static IAudioRenderClient*pRenderClient = NULL;
static WORD nBlockAlign = 1u;
static BOOL bStarted = FALSE;
static void*pIntegerBuffer = NULL;
static void*pFloatBuffer = NULL;
static size_t nBufferSize = 0x10000;
static int fmt = 0;

static PBYTE Convert(PBYTE pbInt, PBYTE pbFloat, DWORD cbFloatLength)
{
	for (DWORD cbIntLength = 0u; cbIntLength < cbFloatLength; cbIntLength += sizeof(__m256)) {
		__m256 a = _mm256_load_ps((float*)(pbFloat + cbIntLength));
		a = _mm256_min_ps(a, _mm256_set_ps(1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f));
		a = _mm256_max_ps(a, _mm256_set_ps(-1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f));
		__m256 b = _mm256_set_ps(0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
		__m256 c = _mm256_cmp_ps(a, b, _CMP_LT_OS);
		b = _mm256_set_ps(0x100, 0x100, 0x100, 0x100, 0x100, 0x100, 0x100, 0x100);
		__m256 d = _mm256_and_ps(b, c);
		c = _mm256_set_ps(0x7fffff00, 0x7fffff00, 0x7fffff00, 0x7fffff00, 0x7fffff00, 0x7fffff00, 0x7fffff00, 0x7fffff00);
		b = _mm256_add_ps(c, d);
		c = _mm256_mul_ps(a, b);
		_mm256_store_si256((__m256i*)(pbInt + cbIntLength), _mm256_cvtps_epi32(c));
	}
	return pbInt;
}

static int control(int cmd, void *arg)
{
	return CONTROL_UNKNOWN;
}

static int init(int rate, int channels, int format, int flags)
{
	HRESULT hr;
	IMMDeviceEnumerator*pEnumerator = NULL;
	IMMDevice*pEndpoint = NULL;
	WAVEFORMATEXTENSIBLE wfx;
	WORD wBitsPerSample;
	WORD nChannels;
	DWORD nSamplesPerSec;
	BOOL got = FALSE;
	REFERENCE_TIME hnsDefaultDevicePeriod;
	REFERENCE_TIME hnsMinimumDevicePeriod;

	hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (hr != S_OK && hr != S_FALSE) {
		mp_msg(MSGT_AO, MSGL_ERR, "Can't Initialize the COM library\n");
		return FALSE;
	}

	hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, (LPVOID*)&pEnumerator);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "Can't Create MMDeviceEnumerator\n");
		return FALSE;
	}

	hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnumerator, eRender, eConsole, &pEndpoint);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "Can't Get Default Audio Endpoint\n");
		goto failure;
	}

	hr = IMMDevice_Activate(pEndpoint, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (LPVOID*)&pAudioClient);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "Can't Activate Endpoint\n");
		goto failure;
	}

	wBitsPerSample = format == AF_FORMAT_FLOAT_NE ? 32 : 16;
	nChannels = 2;
	nBlockAlign = nChannels * wBitsPerSample / 8u;
	nSamplesPerSec = rate;

	wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	wfx.Format.nChannels = nChannels;
	wfx.Format.nSamplesPerSec = nSamplesPerSec;
	wfx.Format.nAvgBytesPerSec = nBlockAlign * nSamplesPerSec;
	wfx.Format.nBlockAlign = nBlockAlign;
	wfx.Format.wBitsPerSample = wBitsPerSample;
	wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	wfx.Samples.wValidBitsPerSample = format == AF_FORMAT_FLOAT_NE ? 24 : wBitsPerSample;
	wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

	hr = IAudioClient_IsFormatSupported(pAudioClient, AUDCLNT_SHAREMODE_EXCLUSIVE, &wfx.Format, NULL);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "Format Is Not Supported\n");
		goto failure;
	}

	hr = IAudioClient_Initialize(pAudioClient, AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 10000000, 0, &wfx.Format, NULL);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "Can't Initialize Audio Client\n");
		goto failure;
	}

	hr = IAudioClient_GetService(pAudioClient, &IID_IAudioRenderClient, (LPVOID*)&pRenderClient);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "Can't Get Service\n");
		goto failure;
	}

	ao_data.samplerate = wfx.Format.nSamplesPerSec;
	ao_data.channels = wfx.Format.nChannels;
	ao_data.format = format == AF_FORMAT_FLOAT_NE ? AF_FORMAT_FLOAT_NE : AF_FORMAT_S16_NE;
	ao_data.bps = wfx.Format.nAvgBytesPerSec;
	ao_data.outburst = nBlockAlign * OUTBURST;
	if (ao_data.buffersize < 0)
		ao_data.buffersize = wfx.Format.nAvgBytesPerSec;

	hr = IAudioClient_GetDevicePeriod(pAudioClient, &hnsDefaultDevicePeriod, &hnsMinimumDevicePeriod);
	if (hr == S_OK) {
		ao_data.outburst = hnsMinimumDevicePeriod * wfx.Format.nAvgBytesPerSec / 10000000;
	}

	if (format == AF_FORMAT_FLOAT_NE) {
		pIntegerBuffer = _aligned_malloc(nBufferSize, 32);
		pFloatBuffer = _aligned_malloc(nBufferSize, 32);
	}

	fmt = format;
	got = TRUE;
	goto success;

failure:
	if (pAudioClient)IAudioClient_Release(pAudioClient);
	pAudioClient = NULL;
	if (pRenderClient)IAudioRenderClient_Release(pRenderClient);
	pRenderClient = NULL;
	bStarted = FALSE;
	fmt = 0;
	if (pIntegerBuffer)_aligned_free(pIntegerBuffer);
	pIntegerBuffer = NULL;
	if (pFloatBuffer)_aligned_free(pFloatBuffer);
	pFloatBuffer = NULL;
success:
	if (pEndpoint)
		IMMDevice_Release(pEndpoint);
	if (pEnumerator)
		IMMDeviceEnumerator_Release(pEnumerator);
	return got;
}

static void reset(void)
{
	HRESULT hr;
	hr = IAudioClient_Stop(pAudioClient);
	if (hr != S_OK && hr != S_FALSE)
		mp_msg(MSGT_AO, MSGL_WARN, "IAudioClient::Stop:%lx\n", hr);
	hr = IAudioClient_Reset(pAudioClient);
	if (hr != S_OK && hr != S_FALSE)
		mp_msg(MSGT_AO, MSGL_WARN, "IAudioClient::Reset:%lx\n", hr);
	bStarted = FALSE;
}

static void audio_pause(void)
{
	HRESULT hr;
	hr = IAudioClient_Stop(pAudioClient);
	if (hr != S_OK && hr != S_FALSE)
		mp_msg(MSGT_AO, MSGL_WARN, "IAudioClient::Stop:%lx\n", hr);
	else
		bStarted = FALSE;
}

static void audio_resume(void)
{
	HRESULT hr;
	hr = IAudioClient_Start(pAudioClient);
	if (hr != S_OK && hr != AUDCLNT_E_NOT_STOPPED) {
		mp_msg(MSGT_AO, MSGL_WARN, "IAudioClient::Start:%lx\n", hr);
	} else
		bStarted = TRUE;
}

static void uninit(int immed)
{
	if (!immed) Sleep(get_delay()*1000.f);
	IAudioClient_Stop(pAudioClient);
	IAudioClient_Release(pAudioClient);
	pAudioClient = NULL;
	IAudioRenderClient_Release(pRenderClient);
	pRenderClient = NULL;
	bStarted = FALSE;
	if (pIntegerBuffer)_aligned_free(pIntegerBuffer);
	pIntegerBuffer = NULL;
	if (pFloatBuffer)_aligned_free(pFloatBuffer);
	pFloatBuffer = NULL;
}

static int get_space(void)
{
	HRESULT hr;
	UINT32 numFramesPadding;
	UINT32 bufferFrameCount;
	UINT32 bytes;

	hr = IAudioClient_GetCurrentPadding(pAudioClient, &numFramesPadding);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "IAudioClient::GetCurrentPadding:%lx\n", hr);
		return 0;
	}

	hr = IAudioClient_GetBufferSize(pAudioClient, &bufferFrameCount);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "IAudioClient::GetBufferSize:%lx\n", hr);
		return 0;
	}

	bytes = (bufferFrameCount - numFramesPadding)*nBlockAlign;
	return fmt == AF_FORMAT_FLOAT_NE ? (bytes >> 5) << 5 : bytes;
}

static int play(void* data, int len, int flags)
{
	HRESULT hr;
	UINT32 numFramesPadding;
	UINT32 bufferFrameCount;
	BYTE *pData;
	UINT32 numFramesAvailable;

	hr = IAudioClient_GetCurrentPadding(pAudioClient, &numFramesPadding);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "IAudioClient::GetCurrentPadding:%lx\n", hr);
		return 0;
	}

	hr = IAudioClient_GetBufferSize(pAudioClient, &bufferFrameCount);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "IAudioClient::GetBufferSize:%lx\n", hr);
		return 0;
	}

	numFramesAvailable = bufferFrameCount - numFramesPadding;
	hr = IAudioRenderClient_GetBuffer(pRenderClient, numFramesAvailable, &pData);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "IAudioRenderClient::GetBuffer:%lx\n", hr);
		return 0;
	}

	if (numFramesAvailable*nBlockAlign < len)
		len = numFramesAvailable * nBlockAlign;
	if (fmt == AF_FORMAT_FLOAT_NE) {
		len >>= 5;
		len <<= 5;
		if (len > nBufferSize) {
			nBufferSize = len;
			_aligned_free(pFloatBuffer);
			_aligned_free(pIntegerBuffer);
			pIntegerBuffer = _aligned_malloc(nBufferSize, 32);
			pFloatBuffer = _aligned_malloc(nBufferSize, 32);
		}
		memcpy(pFloatBuffer, data, len);
		data = Convert((PBYTE)pIntegerBuffer, (PBYTE)pFloatBuffer, len);
	}
	memcpy(pData, data, len);

	hr = IAudioRenderClient_ReleaseBuffer(pRenderClient, len / nBlockAlign, 0);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "IAudioRenderClient::ReleaseBuffer:%lx\n", hr);
		return 0;
	}

	if (!bStarted) {
		hr = IAudioClient_Start(pAudioClient);
		if (hr != S_OK && hr != AUDCLNT_E_NOT_STOPPED) {
			mp_msg(MSGT_AO, MSGL_ERR, "IAudioClient::Start:%lx\n", hr);
			return 0;
		}
		bStarted = TRUE;
	}

	return len;
}

static float get_delay(void)
{
	HRESULT hr;
	UINT32 numFramesPadding;
	hr = IAudioClient_GetCurrentPadding(pAudioClient, &numFramesPadding);
	if (hr != S_OK) {
		mp_msg(MSGT_AO, MSGL_ERR, "IAudioClient::GetCurrentPadding:%lx\n", hr);
		return 0.0f;
	}
	return (float)(numFramesPadding*nBlockAlign) / (float)ao_data.bps;
}
