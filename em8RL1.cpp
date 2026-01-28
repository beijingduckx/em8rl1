//
//  CZ-8RL1 emulator 
//  - Windows side (Tape mechanism emulation part) program
//

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "Recorder.h"

#include "fx2load.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <stdint.h>
#include <stdio.h>

#ifndef _WIN32
#define USE_IMGUI_FILE_DIALOG
#endif

#ifdef USE_IMGUI_FILE_DIALOG
#include "ImGuiFileDialog.h"
#else
#include <Windows.h>
#include <commdlg.h>
#include <io.h>
#endif

#include <libusb.h>
#include <assert.h>
#include <stdlib.h>
#include <locale.h>
#include <map>
#include <filesystem>
#include <chrono>

using namespace std;
using namespace std::filesystem;

void finalize(void);

#define _MAKE_TITLE(A)  A##"CZ-8RL1 Emulator"
#define APP_TITLE      _MAKE_TITLE(L)
#define APP_TITLE_U8   _MAKE_TITLE(u8)

#define NO_TAPE   "NO TAPE"

#define VID 0x04b4
#define PID 0x8613

#define TARGET_FPS  10

#ifdef _WIN32
HWND h_main_window = NULL;
#define MESSAGE(x)   ::MessageBoxA(NULL, x, "Error", MB_OK);
#else
#define MESSAGE(x)   puts(x);
#endif

static uint32_t tape_event;

static bool is_alt_44k = false;
static bool is_rec_bit_convert = false;
static bool is_tape_set = false;
static path tape_filepath(NO_TAPE);

#ifdef USE_IMGUI_FILE_DIALOG
static path last_dialogpath(".");
#endif

static volatile int ui_run_flag = 1;
DataRecorder recorder;
static libusb_device_handle* usb_handle = NULL;

const std::chrono::milliseconds frameDuration(1000 / TARGET_FPS);

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
#ifdef USE_IMGUI_FILE_DIALOG
	IGFD::FileDialogConfig config;
	config.path = last_dialogpath.parent_path().string();
	config.flags = ImGuiFileDialogFlags_DisableCreateDirectoryButton;

	const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
	ImGuiFileDialog::Instance()->OpenDialog("ChooseFileDlgKey", "Choose Tape File", ".tap", config);
#else
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
		std::filesystem::path filepath(file_name);
		tape_filepath = file_name;
		if (recorder.set_tape(filepath) == false) {
			tape_filepath = path(NO_TAPE);
			return;
		};
		is_tape_set = true;
	}
#endif
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

uint32_t draw_run(void* arg) {
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
		MESSAGE("SDL could not initialize");
		return -1;
	}
	// Create window
	window = SDL_CreateWindow(APP_TITLE_U8, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (window == NULL) {
		MESSAGE("Window could not be created");
		return -1;
	}

#ifdef _WIN32
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	SDL_GetWindowWMInfo(window, &info);
	h_main_window = info.info.win.window;
#endif

	tape_event = SDL_RegisterEvents(1);
	recorder.set_event_callback(handle_recorder_event);

	Renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
	if (Renderer == NULL) {
		MESSAGE("Could not create renderer");
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
	ImGui_ImplSDLRenderer2_Init(Renderer);

	SDL_Event e;
	bool is_tape_running = false;

	char font_path[256];
#ifdef _WIN32
	::GetWindowsDirectoryA(font_path, sizeof(font_path));
	string str_font_path(font_path);
	str_font_path.append("\\fonts\\meiryo.ttc");

	if(std::filesystem::exists(str_font_path)){
		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->AddFontFromFileTTF(str_font_path.c_str(), 18, NULL, io.Fonts->GetGlyphRangesJapanese());
	}
#endif

	const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
	std::string message;
	bool popupRequested = false;

	while (ui_run_flag) {
		auto frameStart = std::chrono::steady_clock::now();

		//SDL_WaitEvent(&e);
		while(SDL_PollEvent(&e)) {
			ImGui_ImplSDL2_ProcessEvent(&e);

			if (e.type == SDL_QUIT) {
				ui_run_flag = 0;
			}
			else if (e.type == tape_event) {
				switch (e.user.code) {
				case DataRecorder::EVENT_TAPE_EJECT:
					//::OutputDebugStringA("Eject event");
					handle_eject_tape(true);
					break;
				case DataRecorder::EVENT_USB_DISCONNECTED:
					message = "USB Disconnected";
					popupRequested = true;
					break;
				case DataRecorder::EVENT_USB_ERROR:
					message = "USB Error";
					popupRequested = true;
					break;

				default:
					break;
				}
			}
		}
		is_tape_running = recorder.is_running();

		ImGui_ImplSDLRenderer2_NewFrame();
		ImGui_ImplSDL2_NewFrame();

		ImGui::NewFrame();

		//
		if(popupRequested) {
			ImGui::OpenPopup("Error");
			popupRequested = false;
		}

		// Set default window size & pos
		ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x, main_viewport->WorkPos.y + 20), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2((float)(main_viewport->WorkSize.x / 2.0),(float)(main_viewport->WorkSize.y / 2.0)), ImGuiCond_FirstUseEver);

		ImGui::Begin("CZ-8RL1");

		if (ImGui::BeginMainMenuBar()) {
			if (ImGui::BeginMenu("File", !is_tape_running)) {
				if (ImGui::MenuItem("Set tape..")) {
					handle_eject_tape();
					handle_set_tape();
				}
				if (ImGui::MenuItem("Eject", NULL, false, is_tape_set)){
					handle_eject_tape();
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Exit")) {
					handle_eject_tape();
					ui_run_flag = false;
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
				if (ImGui::MenuItem("Write protect", NULL, recorder.get_tape_write_protected())) {
					recorder.try_set_tape_write_protect(!recorder.get_tape_write_protected());
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}

		ImGui::Text(tape_filepath.filename().u8string().c_str());
		if (is_tape_set == true) {

			bool write_protect = !(recorder.get_sensor() & recorder.TAPE_NOT_WRITE_PROTECT);
			if (write_protect) {
				ImGui::SameLine();
				ImGui::Text(" (WriteProtected)");
			}

			ImGui::Text(tape_mode_map[recorder.get_current_mode()]);
			uint32_t total_count = recorder.get_total_counter();
			if (total_count != 0) {
				ImGui::ProgressBar((float)recorder.get_counter() / recorder.get_total_counter());
			}
			if (is_tape_running == true && ImGui::Button("Stop")) {
				recorder.command(DataRecorder::COM_STOP);
			}
			if (is_tape_running == false && ImGui::Button("TOP")) {
				recorder.command(DataRecorder::COM_TAPETOP);
			}
			if (is_tape_running == false && (ImGui::SameLine(), ImGui::Button("REW"))) {
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

		// Error Message Box
		if(ImGui::BeginPopupModal("Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text(message.c_str());
			if(ImGui::Button("OK")) {
				ImGui::CloseCurrentPopup();
				ui_run_flag = false;
			}
			ImGui::EndPopup(); 
		}

#ifdef USE_IMGUI_FILE_DIALOG
		// FileDialog
		ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + 20, main_viewport->WorkPos.y + 30), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2((float)(main_viewport->WorkSize.x / 1.25),(float)(main_viewport->WorkSize.y / 1.5)), ImGuiCond_FirstUseEver);
		if(ImGuiFileDialog::Instance()->Display("ChooseFileDlgKey")) {
			if(ImGuiFileDialog::Instance()->IsOk()){
				tape_filepath = ImGuiFileDialog::Instance()->GetFilePathName();
				last_dialogpath = tape_filepath;
				if (recorder.set_tape(reinterpret_cast<const char*>(tape_filepath.c_str())) == false) {
					tape_filepath = path(NO_TAPE);
				}
				else {
					is_tape_set = true;
				}
			}
			ImGuiFileDialog::Instance()->Close();
		}
#endif

		ImGui::Render();
		SDL_RenderClear(Renderer);
		ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), Renderer);
		SDL_RenderPresent(Renderer);
		auto frameEnd = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - frameStart);
		if (elapsed < frameDuration) {
			std::this_thread::sleep_for(frameDuration - elapsed);
		}
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
	//ret = libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
	ret = libusb_set_option(NULL, LIBUSB_OPTION_USE_USBDK);
	usb_handle = libusb_open_device_with_vid_pid(NULL, VID, PID);

	if (usb_handle == NULL) {
		MESSAGE("EZ-USB is not connected.");
		return -1;
	}

	if(libusb_kernel_driver_active(usb_handle, 0)) {
		libusb_detach_kernel_driver(usb_handle, 0);
	}
	ret = libusb_claim_interface(usb_handle, 0);
	if (ret < 0) {
		MESSAGE("USB claim interface failed.");
		return -1;
	}
	ret = libusb_set_interface_alt_setting(usb_handle, 0, 1);
	if (ret < 0) {
		MESSAGE("USB interface not found.");
		return -1;
	}

	// Load firmware
	ret = usb_load_firmware(usb_handle);
	if(ret < 0) {
		MESSAGE("Firmware downloading failed.");
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
