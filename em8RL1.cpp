//
//  CZ-8RL1 emulator 
//  - Windows side (Tape mechanism emulation part) program
//

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer.h"

#include "Recorder.h"

#include "fx2load.h"

#include <SDL.h>
#include <SDL_syswm.h>

#define _POSIX_SOURCE
#include <fcntl.h>
#include <io.h>

#include <stdint.h>
#include <stdio.h>
#include <Windows.h>
#include <commdlg.h>
#include <libusb.h>
#include <assert.h>
#include <stdlib.h>
#include <locale.h>
#include <map>
#include <filesystem>

using namespace std;
using namespace std::filesystem;

void finalize(void);

#define _MAKE_TITLE(A)  A##"CZ-8RL1 Emulator"
#define APP_TITLE      _MAKE_TITLE(L)
#define APP_TITLE_U8   _MAKE_TITLE(u8)

#define VID 0x04b4
#define PID 0x8613

HWND h_main_window = NULL;

static uint32_t tape_event;

static bool is_alt_44k = false;
static bool is_rec_bit_convert = false;
static bool is_tape_set = false;
static path tape_filepath("NO TAPE");

static volatile int ui_run_flag = 1;
DataRecorder recorder;
static libusb_device_handle* usb_handle = NULL;


void handle_sample_rate_change(void)
{
	recorder.set_alt_44k(is_alt_44k);
}

void handle_rec_strategy_change(bool use_bit_conversion)
{
	recorder.set_rec_strategy(use_bit_conversion);
}

void handle_set_tape(void)
{
	OPENFILENAME ofn;
	wchar_t file_name[MAX_PATH];

	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = h_main_window;
	ofn.lpstrFile = file_name;
	ofn.lpstrFile[0] = '\0';
	ofn.nMaxFile = sizeof(file_name);
	ofn.lpstrFilter = L"TAP File\0*.tap\0";
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

	if (::GetOpenFileName(&ofn) == TRUE) {
		errno_t ret;
		size_t convertedLen;
		char u8_file_name[MAX_PATH];
		ret = wcstombs_s(&convertedLen, u8_file_name, sizeof(u8_file_name), file_name, sizeof(u8_file_name) - 1);
		tape_filepath = u8_file_name;
		if (recorder.set_tape(file_name) == false) {
			return;
		};
		is_tape_set = true;
	}
}

void handle_eject_tape(bool is_event = false)
{
	if (is_event == false) {
		recorder.eject_tape();
	}
	tape_filepath = u"NO TAPE";
	is_tape_set = false;
}

void handle_recorder_event(uint8_t code) 
{
	SDL_Event event;

	SDL_memset(&event, 0, sizeof(event));
	event.type = tape_event;
	event.user.code = code;
	SDL_PushEvent(&event);
}

DWORD WINAPI draw_run(void* arg) {
	// The window we'll be rendering to
	SDL_Window* window = NULL;

	// The surface contained by the window
	SDL_Renderer* Renderer = NULL;

	auto set_title = [&]() {
		char tmp[100];
		snprintf(tmp, sizeof(tmp), APP_TITLE_U8);
		SDL_SetWindowTitle(window, tmp);
	};

	// Initialize SDL
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		::MessageBox(NULL, L"SDL could not initialize", APP_TITLE, MB_OK);
		return -1;
	}
	// Create window
	window = SDL_CreateWindow(APP_TITLE_U8, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (window == NULL) {
		::MessageBox(NULL, L"Window could not be created", APP_TITLE, MB_OK);
		return -1;
	}

	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	SDL_GetWindowWMInfo(window, &info);
	h_main_window = info.info.win.window;

	tape_event = SDL_RegisterEvents(1);
	recorder.set_event_callback(handle_recorder_event);

	Renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
	if (Renderer == NULL) {
		::MessageBox(NULL, L"Could not create renderer", APP_TITLE, MB_OK);
		return -1;
	}
	SDL_SetRenderDrawColor(Renderer, 0x00, 0x00, 0x00, 0xff);
	SDL_RenderClear(Renderer);

	std::map<DataRecorder::tape_mode_t, const char*> tape_mode_map = {
		{DataRecorder::TAPE_MODE_PLAY, "LOAD"},
		{DataRecorder::TAPE_MODE_STOP, "STOP"},
		{DataRecorder::TAPE_MODE_REC, "SAVE"},
		{DataRecorder::TAPE_MODE_REW, "REW"},
		{DataRecorder::TAPE_MODE_FF, "FF"},
		{DataRecorder::TAPE_MODE_AREW, "AREW"},
		{DataRecorder::TAPE_MODE_AFF, "AFF"},
		{DataRecorder::TAPE_MODE_NONE, "??"},
		{DataRecorder::TAPE_MODE_EJECT, "EJECT"},

	};

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

		//ImGui::StyleColorsDark();
	ImGui::StyleColorsLight();

	// Setup Platform/Renderer backends
	ImGui_ImplSDL2_InitForSDLRenderer(window, Renderer);
	ImGui_ImplSDLRenderer_Init(Renderer);

	SDL_Event e;
	bool is_tape_running = false;

	char font_path[256];
	::GetWindowsDirectoryA(font_path, sizeof(font_path));
	string str_font_path(font_path);
	str_font_path.append("\\fonts\\meiryo.ttc");

	if(std::filesystem::exists(str_font_path)){
		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->AddFontFromFileTTF(str_font_path.c_str(), 18, NULL, io.Fonts->GetGlyphRangesJapanese());
	}

	while (ui_run_flag) {

		SDL_WaitEvent(&e);
		//		SDL_PollEvent(&e);
		ImGui_ImplSDL2_ProcessEvent(&e);

		is_tape_running = recorder.is_running();
		if (e.type == SDL_QUIT) {
			ui_run_flag = 0;
		}
		else if (e.type == tape_event) {
			switch (e.user.code) {
			case DataRecorder::EVENT_TAPE_EJECT:
				::OutputDebugStringA("Eject event");
				handle_eject_tape(true);
				break;
			case DataRecorder::EVENT_USB_DISCONNECTED:
				::MessageBox(h_main_window, L"USB disconnected", APP_TITLE, MB_OK);
				break;
			case DataRecorder::EVENT_USB_ERROR:
				::MessageBox(h_main_window, L"USB error", APP_TITLE, MB_OK);
				break;

			default:
				break;
			}
		}
		ImGui_ImplSDLRenderer_NewFrame();
		ImGui_ImplSDL2_NewFrame();

		ImGui::NewFrame();

		const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + 10, main_viewport->WorkPos.y + 30), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2((float)(main_viewport->WorkSize.x / 2.0),(float)(main_viewport->WorkSize.y / 2.0)), ImGuiCond_FirstUseEver);

		ImGui::Begin("Status");

		if (ImGui::BeginMainMenuBar()) {
			if (ImGui::BeginMenu("File", !is_tape_running)) {
				if (ImGui::MenuItem("Set tape..")) {
					handle_eject_tape();
					handle_set_tape();
				}
				if (ImGui::MenuItem("Eject", NULL, false, is_tape_set)){
					handle_eject_tape();
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Settings", !is_tape_running)) {
				if (ImGui::MenuItem("Alt. 44kHz frequency", NULL, is_alt_44k)) {
					is_alt_44k = !is_alt_44k;
					handle_sample_rate_change();
				}
				if (ImGui::MenuItem("Bit conversion on Save", NULL, is_rec_bit_convert)) {
					is_rec_bit_convert = !is_rec_bit_convert;
					handle_rec_strategy_change(is_rec_bit_convert);
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}

		ImGui::Text(tape_filepath.filename().u8string().c_str());
		if (is_tape_set == true) {
			ImGui::Text(tape_mode_map[recorder.get_current_mode()]);
			uint32_t total_count = recorder.get_total_counter();
			if (total_count != 0) {
				ImGui::ProgressBar((float)recorder.get_counter() / recorder.get_total_counter());
			}
			if (is_tape_running == true && ImGui::Button("Stop")) {
				recorder.command(DataRecorder::COM_STOP);
			}
			if (is_tape_running == false && ImGui::Button("REW")) {
				recorder.command(DataRecorder::COM_REW);
			}
			if (is_tape_running == false && (ImGui::SameLine() , ImGui::Button("FF"))) {
				recorder.command(DataRecorder::COM_FF);
			}
			if (is_tape_running == false && (ImGui::SameLine(), ImGui::Button("AREW"))) {
				recorder.command(DataRecorder::COM_AREW);
			}
			if (is_tape_running == false && (ImGui::SameLine(), ImGui::Button("AFF"))) {
				recorder.command(DataRecorder::COM_AFF);
			}
			ImGui::Text("Counter: %d", recorder.get_counter() / 8);
		}
		ImGui::End();

		ImGui::Render();
		SDL_RenderClear(Renderer);
		ImGui_ImplSDLRenderer_RenderDrawData(ImGui::GetDrawData());
		SDL_RenderPresent(Renderer);
	}

	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}


//======================================================================
// Main
//======================================================================
int main(int argc, char* argv[]) {
	int ret;

	setlocale(LC_CTYPE, ".UTF8");

	// Initialize USB
	ret = libusb_init(NULL);
	ret = libusb_set_option(NULL, LIBUSB_OPTION_USE_USBDK);
	usb_handle = libusb_open_device_with_vid_pid(NULL, VID, PID);

	if (usb_handle == NULL) {
		::MessageBox(NULL, L"EZ-USB is not connected.", APP_TITLE, MB_OK);
		return -1;
	}
	ret = libusb_claim_interface(usb_handle, 0);
	ret = libusb_set_interface_alt_setting(usb_handle, 0, 1);
	if (ret < 0) {
		::MessageBox(NULL, L"USB interface not found", APP_TITLE, MB_OK);
		return -1;
	}

	// Load firmware
	ret = usb_load_firmware(usb_handle);
	if(ret < 0) {
		::MessageBox(NULL, L"Firmware downloading failed.", APP_TITLE, MB_OK);
		return -1;
	}

	// Init data recorder usb
	recorder.set_usb_handle(usb_handle);
	recorder.set_rec_strategy(is_rec_bit_convert);

	recorder.power_on();

	// Start GUI
	draw_run(NULL);

	// End of the code
	finalize();

	return 0;
}

void finalize() {
	recorder.power_off();
	libusb_close(usb_handle);
}
