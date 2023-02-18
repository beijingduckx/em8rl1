#pragma once

#include <stdio.h>
#include <libusb.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

class BitStream {
public:
	BitStream(void) {
		m_byte_data = nullptr;
		m_byte_length = 0;

		m_bit_offset = 0;
		m_byte_offset = 0;
		m_mask = 0x80;
		m_current_byte = 0;
		m_dirty = false;
	}

	~BitStream() {
		if (m_byte_data != nullptr) {
			delete[] m_byte_data;
		}
	}

	void set_bit_pos(uint32_t pos)
	{
		m_byte_offset = pos / 8;
		m_bit_offset = pos % 8;
		update_current_byte();
	}

	uint32_t get_bit_pos(void)
	{
		return (uint32_t)(m_byte_offset * 8) + m_bit_offset;
	}

	virtual void set_byte_stream(uint8_t* data, size_t length) {
		if (m_byte_data != nullptr) {
			delete[] m_byte_data;
			m_byte_data = nullptr;
		}
		m_byte_data = new uint8_t[length];
		memcpy(m_byte_data, data, length);
		m_byte_length = length;

		m_bit_offset = 0;
		m_byte_offset = 0;
		m_mask = 0x80;

		m_current_byte = m_byte_data[m_byte_offset];
	}

	uint8_t get_bit(void) {
		if (m_current_byte & m_mask) {
			return 1;
		}
		else {
			return 0;
		}
	}

	uint8_t get_invert_bit(void) {
		if (m_current_byte & m_mask) {
			return 0;
		}
		else {
			return 1;
		}
	}

	void write_bit(uint8_t bit) {
		if (bit) {
			m_current_byte |= m_mask;
		}
		else {
			m_current_byte &= ~m_mask;
		}
		m_dirty = true;
	}

	void flush(void)
	{
		if (m_dirty == true) {
			write_current_byte();
		}
	}

	int move_forward(void) {
		m_bit_offset++;
		if (m_bit_offset == 8) {
			if (m_dirty == true) {
				write_current_byte();
			}
			m_bit_offset = 0;
			m_mask = 0x80;
			m_byte_offset++;
			if (m_byte_offset >= (int)m_byte_length) {
				m_mask = 0x01;
				m_bit_offset = 7;
				m_byte_offset--;
				return -1;
			}
			update_current_byte();
		}
		else {
			m_mask >>= 1;
		}
		return 0;
	}

	int move_backward(void) {
		m_bit_offset--;
		if (m_bit_offset < 0) {
			if (m_dirty == true) {
				write_current_byte();
			}
			m_bit_offset = 7;
			m_mask = 0x01;
			m_byte_offset--;
			if (m_byte_offset < 0) {
				m_byte_offset = 0;
				return -1;
			}
			update_current_byte();
		}
		else {
			m_mask <<= 1;
		}
		return 0;
	}


protected:
	virtual void update_current_byte(void)
	{
		m_dirty = false;
		m_current_byte = m_byte_data[m_byte_offset];
	}

	virtual void write_current_byte(void) {
		m_byte_data[m_byte_offset] = m_current_byte;
		m_dirty = false;
	}


	uint8_t* m_byte_data;
	size_t m_byte_length;

	uint8_t m_current_byte;
	int m_byte_offset;
	int m_bit_offset;
	uint8_t m_mask;
	bool m_dirty;
};

class FileBitStream : public BitStream {
public:
	void set_byte_stream(int file_handle, int header_offset) {
		m_file = file_handle;
		m_bit_offset = 0;
		m_byte_offset = 0;
		m_mask = 0x80;
		m_header_offset = header_offset;

		struct _stat stat_data;
		_fstat(m_file, &stat_data);
		m_byte_length = stat_data.st_size;

		update_current_byte();
	}

protected:
	void update_current_byte(void)
	{
		m_dirty = false;
		_lseek(m_file, m_byte_offset + m_header_offset, SEEK_SET);
		_read(m_file, &m_current_byte, 1);
	}

	void write_current_byte(void) {
		_write(m_file, &m_current_byte, 1);
		m_dirty = false;
	}

	int m_file;
	int m_header_offset;
};


class TapFile {
public:
	TapFile(void) {
		m_usb_sample_rate = EZUSB_SAMPLE_RATE;
		m_usb_time = 0;
		m_continue = false;
		m_file = 0;

		m_apss_bit = 0;
		m_apss_bit_change_detected = false;
		m_apss_detect_count = 0;
		m_apss_ignore_count = 0;
		m_apss_first_bit = 0;
		m_apss_count = 0;

		m_noise_count = 0;
		m_noise_limit = 0;
		m_tape_hz = 0;

		m_file_readonly = false;
		m_old_format = false;

		m_rec_bit_conversion = false;
		m_tape_end = false;
	}

	~TapFile() {
		if (m_file != 0) {
			_close(m_file);
		}
	}

	bool open(wchar_t* filename) {
		// close if opened
		close();

		// check read-only
		struct _stat stat_data;
		int oflag = _O_BINARY;
		int ret = _wstat(filename, &stat_data);
		if (ret < 0) {
			return false;
		}
		if (stat_data.st_mode & S_IWRITE) {
			m_file_readonly = false;
			oflag |= _O_RDWR;
		}
		else {
			m_file_readonly = true;
			oflag |= _O_RDONLY;
		}

		// 
		m_file = _wopen(filename, oflag);
		if (m_file < 0) {
			m_file = 0;
			return false;
		}

		// read header
		ret = _read(m_file, &m_header, sizeof(m_header));
		if (ret < 0) {
			close();
			return false;
		}

		// check if new format
		if (m_header.index == TAPE_INDEX) {
			// new format
			m_header.datasize = (stat_data.st_size - sizeof(m_header)) * 8;
			m_old_format = false;
		}
		else {
			// old format
			m_header.frequency = m_header.index;
			m_header.datasize = (stat_data.st_size - sizeof(m_header.index)) * 8;
			m_header.position = sizeof(m_header.index) * 8;
			m_header.protect = (m_file_readonly == true) ? TAPE_PROTECT : 0;
			m_old_format = true;
		}

		// initialize member variables
		m_tape_hz = m_header.frequency;
		m_continue = false;

		m_apss_count = 0;
		if (m_tape_hz < 8000 || m_tape_hz > 48000) {
			return false;
		}
		if (m_tape_hz > 44000) {
			m_noise_limit = 2;
		}
		else if (m_tape_hz > 32000) {
			m_noise_limit = 1;
		}
		else {
			m_noise_limit = 0;
		}
		m_apss_detect_count = (int)(m_tape_hz * APSS_DETECT_SEC);

		m_tape_data.set_byte_stream(m_file, get_header_byte_size());
		m_tape_data.set_bit_pos(m_header.position);

		return true;
	}

	void close() {
		if (is_opened()) {
			if (m_old_format == false && m_file_readonly == false) {
				m_header.position = m_tape_data.get_bit_pos();
				_lseek(m_file, 0, 0);
				_write(m_file, &m_header, sizeof(m_header));
			}
			_close(m_file);
		}
		m_file = 0;
	}

	uint32_t get_bit_pos(void)
	{
		if (m_file != 0) {
			return m_tape_data.get_bit_pos();
		}
		return 0;
	}

	uint32_t get_total_bits(void)
	{
		if (is_opened()) {
			return m_header.datasize;
		}
		return 0;
	}

	uint32_t get_tape_sample_rate(void) {
		return m_tape_hz;
	}

	void set_usb_sample_rate(int sample_rate)
	{
		m_usb_sample_rate = sample_rate;
	}

	ssize_t fill_usb_data(uint8_t* usb_data, size_t required) {
		uint8_t usb_byte = 0;
		int usb_data_index = 0;
		int usb_bit_index = 0;

		if (m_continue == false) {
			m_usb_time = m_usb_sample_rate / 2;
		}
		m_continue = false;

		while (1) {
			uint8_t tape_bit = m_tape_data.get_bit();
			while (m_usb_time > 0) {
				usb_byte <<= 1;
				usb_byte |= tape_bit;
				m_usb_time -= m_tape_hz;
				usb_bit_index++;
				if (usb_bit_index == 8) {
					usb_bit_index = 0;
					usb_data[usb_data_index] = usb_byte;
					usb_data_index++;
					if (usb_data_index == required) {
						m_continue = true;
						return required;
					}
				}
			}
			if (m_tape_data.move_forward() < 0) {
				return usb_data_index;
			}
			m_usb_time += m_usb_sample_rate;
		}
	}

	void start_write(void)
	{
		m_continue = true;
		m_tape_end = false;
		std::thread write_thread([this]() {this->write_usb_data_to_tape_thread(); });
		write_thread.swap(m_write_tape_thread);
	}

	void stop_write(void)
	{
		m_continue = false;
		m_write_cond.notify_one();
		m_write_tape_thread.join();
		m_tape_data.flush();
	}

	int write_usb_data_to_tape(uint8_t* data, size_t length)
	{
		// FIXME: Ugly way to inform the tape end...
		if (m_tape_end == true) {
			return -1;
		}
		// FIXME: push data into vector 
		m_usb_data.set_byte_stream(data, length);
		m_write_cond.notify_one();
		return 0;
	}

	int rewind(int msec)
	{
		int ret = 0;
		int bits = (m_tape_hz / 1000) * msec * FAST_MODE_MULTIPLY;
		m_continue = false;

		for (int i = 0; i < bits; i++) {
			ret = m_tape_data.move_backward();
			if (ret != 0) {
				return ret;
			}
		}
		return 0;
	}

	int ff(int msec)
	{
		int ret = 0;
		int bits = (m_tape_hz / 1000) * msec * FAST_MODE_MULTIPLY;
		m_continue = false;

		for (int i = 0; i < bits; i++) {
			ret = m_tape_data.move_forward();
			if (ret != 0) {
				return ret;
			}
		}
		return 0;
	}

	void start_apss(void) {
		m_apss_count = 0;
		m_noise_count = 0;
		m_apss_bit_change_detected = false;
		m_apss_first_bit = true;

		m_continue = false;
		m_apss_ignore_count = (int)(m_tape_hz * APSS_IGNORE_SEC);
	}

	void start_aff(void)
	{
		start_apss();
	}

	void start_arew(void)
	{
		start_apss();
	}

	int apss_bit(uint8_t bit)
	{
		if (m_apss_ignore_count > 0) {
			m_apss_ignore_count--;
			return 0;
		}
		if (m_apss_first_bit == true) {
			m_apss_bit = bit;
			m_apss_first_bit = false;
		}
		if (bit != m_apss_bit) {
			m_noise_count++;
			if (m_noise_count > m_noise_limit) {
				m_apss_bit = bit;
				m_apss_count = 0;
				m_noise_count = 0;
				m_apss_bit_change_detected = true;
			}
		}
		else {
			m_noise_count = 0;
			if (m_apss_bit_change_detected == true) {
				m_apss_count++;
			}
		}
		if (m_apss_count > m_apss_detect_count) {
			return -1;
		}
		return 0;
	}

	int aff(int msec)
	{
		int bits = (m_tape_hz / 1000) * msec * FAST_MODE_MULTIPLY;
		int ret;

		for (int i = 0; i < bits; i++) {
			uint8_t bit = m_tape_data.get_bit();
			ret = apss_bit(bit);
			if (ret != 0) {
				return ret;
			}
			ret = m_tape_data.move_forward();
			if (ret != 0) {
				return ret;
			}
		}
		return 0;
	}

	int arew(int msec)
	{
		int bits = (m_tape_hz / 1000) * msec * FAST_MODE_MULTIPLY;
		int ret;

		for (int i = 0; i < bits; i++) {
			uint8_t bit = m_tape_data.get_bit();
			ret = apss_bit(bit);
			if (ret != 0) {
				return ret;
			}
			ret = m_tape_data.move_backward();
			if (ret != 0) {
				return ret;
			}
		}
		return 0;
	}

	bool is_write_protected(void)
	{
		if (m_header.protect != 0 || m_file_readonly == true) {
			return true;
		}
		else {
			return false;
		}
	}

	void set_rec_bit_conversion(bool use_bit_conversion)
	{
		m_rec_bit_conversion = use_bit_conversion;
	}

private:
	int write_bit(uint8_t bit) {
		int duration;

		//::OutputDebugStringA((bit == 0) ? "0" : "1");
		if (bit == 1) {
			duration = (m_tape_hz / 8000) * 2; // 250u
		}
		else {
			duration = m_tape_hz / 8000; // 125u
		}

		for (int index = 0; index < duration; index++) {
			m_tape_data.write_bit(1);
			if (m_tape_data.move_forward() < 0) {
				return -1;
			}
		}
		for (int index = 0; index < duration; index++) {
			m_tape_data.write_bit(0);
			if (m_tape_data.move_forward() < 0) {
				return -1;
			}
		}
		return 0;
	}

	int write_blank(int usb_bit_count)
	{
		int tape_duration;

		tape_duration = usb_bit_count * (m_tape_hz / 100) / (m_usb_sample_rate / 100);
//		char tmp[256];
//		snprintf(tmp, sizeof(tmp), "\nBlank in tape bits %d\n", tape_duration);
//		::OutputDebugStringA(tmp);
		for (int index = 0; index < tape_duration; index++) {
			m_tape_data.write_bit(0);
			if (m_tape_data.move_forward() < 0) {
				return -1;
			}
		}

		return 0;
	}

	int search_edge(BitStream* stream, bool isRising) {
		uint8_t prev_bit;
		uint8_t current_bit;
		int bit_count = 0;
		uint8_t prev_cond;
		uint8_t current_cond;

		if (isRising) {
			prev_cond = 0;
			current_cond = 1;
			prev_bit = 1;
		}
		else {
			prev_cond = 1;
			current_cond = 0;
			prev_bit = 0;
		}

		while (1) {
			bit_count++;
			current_bit = stream->get_invert_bit();
			if (prev_bit == prev_cond && current_bit == current_cond) {
				return bit_count;
			}

			prev_bit = current_bit;

			if (stream->move_forward() < 0) {
				std::unique_lock lk(m_write_lock);
				m_write_cond.wait(lk, [this]() {return (m_usb_data.get_bit_pos() == 0 || m_continue == false); });
				if (m_continue == false) {
					return -bit_count;
				}
			}
		}
	}

	DWORD write_usb_data_to_tape_thread(void)
	{
		uint8_t bit;
		int duration_125us = 8000;
		int judge_duration = m_usb_sample_rate / duration_125us + (m_usb_sample_rate / duration_125us) / 2;

		// Wait for start REC
		{
			std::unique_lock lk(m_write_lock);
			m_write_cond.wait(lk, [this]() {return (m_usb_data.get_bit_pos() == 0 || m_continue == false); });
			if (m_continue == false) {
				return 0;
			}
		}

		if (m_rec_bit_conversion == true || m_tape_hz < 32000) {
			while (1) {
				// search rising edge
				int bit_count = search_edge(&m_usb_data, true);
				if (bit_count < 0) {
					write_blank(-bit_count);
					m_tape_end = true;
					return -1;
				}

				// write blank bits if edge isn't detected within high duration
				if (bit_count > duration_125us * 4) {
					if (write_blank(bit_count) < 0) {
						m_tape_end = true;
						return -1;
					}
				}

				// forward 187.5usec
				for (int index = 0; index < judge_duration; index++) {
					if (m_usb_data.move_forward() < 0) {
						std::unique_lock lk(m_write_lock);
						m_write_cond.wait(lk, [this]() {return (m_usb_data.get_bit_pos() == 0 || m_continue == false); });
						if (m_continue == false) {
							return -1;
						}
					}
				}

				// check H/L
				bit = m_usb_data.get_invert_bit();
				if (write_bit(bit) < 0) {
					m_tape_end = true;
					return -1;
				}
			}
		}
		else {
			// No bit conversion .. simply store the bitstream (with simple decimation)
			int tape_time = m_tape_hz / 2;
			int high_count = 0;
			int bit_count = 0;

			while (1) {
				// Bit-inversion was required for better compatibility
				uint8_t usb_bit = m_usb_data.get_invert_bit();
				if (usb_bit == 1) {
					high_count++;
				}
				bit_count++;
				while (tape_time > 0) {
					if (high_count * 2 > bit_count) {
						usb_bit = 1;
					}
					else if (high_count * 2 == bit_count) {
						// usb_bit = usb_bit;
					}
					else {
						usb_bit = 0;
					}
					bit_count = 0;
					high_count = 0;
					m_tape_data.write_bit(usb_bit);
					tape_time -= m_usb_sample_rate;
					if (m_tape_data.move_forward() < 0) {
						m_tape_end = true;
						return -1;
					}
				}
				if (m_usb_data.move_forward() < 0) {
					std::unique_lock lk(m_write_lock);
					m_write_cond.wait(lk, [this]() {return (m_usb_data.get_bit_pos() == 0 || m_continue == false); });
					if (m_continue == false) {
						return -1;
					}
				}
				tape_time += m_tape_hz;
			}
		}
	}

	int get_header_byte_size(void)
	{
		if (m_old_format == true) {
			return sizeof(m_header.index);
		}
		else {
			return sizeof(X1TAPE_HEADER);
		}
	}

	bool is_opened(void)
	{
		return (m_file != 0);
	}

private:
	// From xmil026_tt143s.zip 
	typedef struct X1TapeHeaderRecoard
	{
		uint32_t index;       /* 00H:識別インデックス "TAPE"            */
		uint8_t name[17];             /* 04H:テープの名前(asciiz)               */
		uint8_t reserve[5];  /* 15H:リザーブ                           */
		uint8_t protect;     /* 1AH:ライトプロテクトノッチ             */
								   /*     (00H=書き込み可、10H=書き込み禁止）*/
		uint8_t format;      /* 1BH:フォーマットの種類 ※未対応        */
								   /*    （01H=定速サンプリング方法）        */
		uint32_t frequency;   /* 1CH:サンプリング周波数(Ｈｚ単位）      */
		uint32_t datasize;    /* 20H:テープデータのサイズ（ビット単位） */
		uint32_t position;    /* 24H:テープの位置（ビット単位）         */
	}X1TAPE_HEADER;

	static constexpr int EZUSB_SAMPLE_RATE = 48000;
	static constexpr int FAST_MODE_MULTIPLY = 18;
	static constexpr float APSS_DETECT_SEC = 3.5;
	static constexpr float APSS_IGNORE_SEC = 3.5;
	static constexpr uint32_t TAPE_INDEX = 0x45504154;
	static constexpr uint8_t TAPE_PROTECT = 0x10;

	int m_usb_sample_rate;
	int m_tape_hz;
	int m_usb_time;

	int m_apss_count;
	int m_noise_count;
	int m_noise_limit;
	int m_apss_ignore_count;
	int m_apss_detect_count;

	bool m_apss_bit_change_detected;
	bool m_apss_first_bit;

	uint8_t m_apss_bit;

	bool m_continue;

	BitStream m_usb_data;
	FileBitStream m_tape_data;

	int m_file;
	X1TAPE_HEADER m_header;
	bool m_file_readonly;
	bool m_old_format;

	bool m_rec_bit_conversion;
	bool m_tape_end;

	std::thread m_write_tape_thread;
	std::mutex m_write_lock;
	std::condition_variable m_write_cond;
};


//
//
//

class DataRecorder {
public:
	DataRecorder(void) {
		m_sensor_state = 0;
		m_tape_mode = TAPE_MODE_EJECT;
		m_tape_run_flag = false;
		m_tape_transfer = libusb_alloc_transfer(0);

		m_usb_handle = nullptr;
		m_usb_callback = this->usb_callback;

		m_is_send_event = true;
		m_event_callback = nullptr;

		m_use_alt_44k = false;

		m_command_receive_run_flag = true;
		m_command_sender_run_flag = true;
		m_usb_error = false;
	}

	enum tape_command_t {
		COM_EJECT = 0x00,
		COM_STOP = 0x01,
		COM_PLAY = 0x02,
		COM_FF = 0x03,
		COM_REW = 0x04,
		COM_AFF = 0x05,
		COM_AREW = 0x06,
		COM_REC = 0x0a,
		COM_STATUS = 0x80,
		COM_SENSOR = 0x81,
	};

	enum tape_mode_t {
		TAPE_MODE_NONE,
		TAPE_MODE_PLAY,
		TAPE_MODE_REC,
		TAPE_MODE_REW,
		TAPE_MODE_FF,
		TAPE_MODE_AREW,
		TAPE_MODE_AFF,
		TAPE_MODE_STOP,
		TAPE_MODE_EJECT
	};

	enum sensor_value_t {
		TAPE_RUNNING = 0x01,
		TAPE_SET = 0x02,
		TAPE_NOT_WRITE_PROTECT = 0x04
	};

	enum recorder_event_t {
		EVENT_TAPE_STOP,
		EVENT_TAPE_EJECT,
		EVENT_UPDATE_SCREEN,
		EVENT_USB_DISCONNECTED,
		EVENT_USB_ERROR,
	};

	void power_on(void)
	{
		start_command_receive_thread();
		start_command_sender_thread();
	}

	void power_off(void)
	{
		eject_tape();
		::Sleep(500);  // wait for stop_tape() process
		m_command_receive_run_flag = false;
		if (m_usb_error == false) {
			libusb_cancel_transfer(m_command_receive_transfer);
		}
		m_command_receive_thread.join();

		m_command_sender_run_flag = false;
		m_command_cond.notify_one();
		m_command_sender_thread.join();

		if (m_usb_thread.joinable() == true) {
			libusb_cancel_transfer(m_tape_transfer);
			m_tape_run_flag = false;
			m_usb_thread.join();
			libusb_free_transfer(m_tape_transfer);
		}

	}

	bool set_tape(wchar_t* file_name) {
		if (m_tape.open(file_name) == false) {
			return false;
		}
		m_sensor_state = TAPE_SET | ((m_tape.is_write_protected() == true) ? 0 : TAPE_NOT_WRITE_PROTECT);
		m_tape_mode = TAPE_MODE_STOP;
		set_usb_sample_rate();
		send_sensor();

		return true;
	}

	void eject_tape(bool is_internal = false) {
		m_tape.close();
		m_sensor_state = 0;
		send_sensor();
		if (is_internal == true) {
			m_event_callback(EVENT_TAPE_EJECT);
		}
	}

	tape_mode_t get_current_mode(void) {
		return m_tape_mode;
	}

	uint8_t get_sensor(void) {
		return m_sensor_state;
	}

	uint32_t get_tape_sample_rate(void)
	{
		return m_tape.get_tape_sample_rate();
	}

	void set_event_callback(void(*fn)(uint8_t)) {
		m_event_callback = fn;
	}

	void set_alt_44k(bool use_alt_44k) {
		m_use_alt_44k = use_alt_44k;
		set_usb_sample_rate();
	}

	void set_rec_strategy(bool use_bit_conversion) {
		m_tape.set_rec_bit_conversion(use_bit_conversion);
	}

	uint32_t get_counter(void) {
		return m_tape.get_bit_pos();
	}

	uint32_t get_total_counter(void) {
		return m_tape.get_total_bits();
	}

	void set_usb_handle(libusb_device_handle* handle)
	{
		m_usb_handle = handle;
	}

	bool is_running(void)
	{
		return m_tape_run_flag;
	}

	void command(uint8_t command) {
		process_command(command);
		// ignore return value

		if (command == COM_STOP) {
			send_response(PC_REQUEST, COM_STOP);
		}
	}

private:
	void stop_tape(bool is_send_event = true)
	{
		if (m_tape_mode == TAPE_MODE_REC) {
			::Sleep(500);// wait for EZ-USB to complete OUT process 
			m_tape_run_flag = false;
			m_tape.stop_write();
			libusb_cancel_transfer(m_tape_transfer);
		}
		// FIXME: clean-up stop tape handling.. 
		m_tape_run_flag = false;
		if (m_usb_thread.joinable() == true) {
			m_usb_thread.join();
		}
		if (is_send_event == true) {
			send_sensor();
			send_response(PC_REQUEST, DataRecorder::COM_STOP);
		}
		m_is_send_event = true;
	}

	bool process_command(uint8_t command) {
		bool is_respond_immediately = true;
		uint8_t new_sensor = m_sensor_state;
		tape_mode_t new_mode = TAPE_MODE_NONE;

		switch (command) {
		case COM_PLAY:
//			::OutputDebugStringA("PLAY\n");
			new_sensor |= TAPE_RUNNING;
			new_mode = TAPE_MODE_PLAY;
			break;
		case COM_STOP:
//			::OutputDebugStringA("STOP\n");
			new_mode = TAPE_MODE_STOP;
			new_sensor &= ~TAPE_RUNNING;
			stop_tape(false);
			break;
		case COM_REW:
//			::OutputDebugStringA("REW\n");
			new_sensor |= TAPE_RUNNING;
			new_mode = TAPE_MODE_REW;
			break;
		case COM_FF:
//			::OutputDebugStringA("FF\n");
			new_sensor |= TAPE_RUNNING;
			new_mode = TAPE_MODE_FF;
			break;
		case COM_AREW:
//			::OutputDebugStringA("AREW\n");
			new_sensor |= TAPE_RUNNING;
			new_mode = TAPE_MODE_AREW;
			m_tape.start_arew();
			is_respond_immediately = false;
			break;
		case COM_AFF:
//			::OutputDebugStringA("AFF\n");
			new_sensor |= TAPE_RUNNING;
			new_mode = TAPE_MODE_AFF;
			m_tape.start_aff();
			is_respond_immediately = false;
			break;
		case COM_EJECT:
			stop_tape(false);
//			::OutputDebugStringA("Eject\n");
			new_mode = TAPE_MODE_EJECT;
			new_sensor &= ~(TAPE_RUNNING | TAPE_SET);
			eject_tape(true);
			break;
		case COM_REC:
//			::OutputDebugStringA("REC\n");
			new_sensor |= TAPE_RUNNING;
			new_mode = TAPE_MODE_REC;
			if (m_tape_mode != TAPE_MODE_REC) {
				m_tape.start_write();
			}
			break;
		default:
//			::OutputDebugStringA("Unknown code..\n");
			break;
		}

		if (m_tape_mode != TAPE_MODE_STOP && m_tape_mode != TAPE_MODE_EJECT && new_mode != m_tape_mode) {
			// To change the tape mode, stop thet tape first
			stop_tape(false);
		}
		m_tape_mode = new_mode;
		m_sensor_state = new_sensor;

		if (m_tape_run_flag == false && (new_sensor & TAPE_RUNNING) && (new_sensor & TAPE_SET)) {
			m_tape_run_flag = true;

			if (m_usb_thread.joinable() == true) {
				m_usb_thread.join();
			}
			std::thread usb_thread([this]() {this->run_tape_thread(); });
			usb_thread.swap(m_usb_thread);
			::OutputDebugStringA("Tape Running..");
		}
		send_sensor();
		m_event_callback(EVENT_UPDATE_SCREEN);

		return is_respond_immediately;
	}

	void run_tape_thread(void) {
		uint8_t trans_data[512];
		int actual_length = 0;
		ULONGLONG prev_time = 0;
		bool is_usb_task = false;
		bool is_send_event = false;

		if (m_usb_error) {
			return;
		}

		while (m_tape_run_flag) {
			usb_callback_user_data_t user_data;
			user_data.completed = 0;
			user_data.recorder = this;
			is_usb_task = false;

			switch (m_tape_mode) {
			case TAPE_MODE_REC:
				libusb_fill_bulk_transfer(m_tape_transfer, m_usb_handle, IN_TAPE_EP, trans_data,
					sizeof(trans_data), m_usb_callback, &user_data, USB_TIMEOUT_MS);
				is_usb_task = true;
				break;

			case TAPE_MODE_PLAY:
			{
				ssize_t num_read = m_tape.fill_usb_data(trans_data, READ_CHUNK_SIZE);
				if (num_read <= 0) {
					m_tape_run_flag = false;
					is_send_event = true;
					num_read = -num_read;
				}
				if (num_read == 0) {
					break;
				}
				libusb_fill_bulk_transfer(m_tape_transfer, m_usb_handle, OUT_TAPE_EP, trans_data,
					(int)num_read, m_usb_callback, &user_data, USB_TIMEOUT_MS);
				is_usb_task = true;
			}
			break;

			case TAPE_MODE_REW:
				if (m_tape.rewind(10) < 0) {
					m_tape_run_flag = false;
					is_send_event = true;
				}
				else {
					::Sleep(10);
				}
				break;

			case TAPE_MODE_FF:
				if (m_tape.ff(10) < 0) {
					m_tape_run_flag = false;
					is_send_event = true;
				}
				else {
					::Sleep(10);
				}
				break;

			case TAPE_MODE_AREW:
				if (m_tape.arew(10) < 0) {
					m_tape_run_flag = false;
					is_send_event = true;
				}
				else {
					::Sleep(10);
				}
				break;

			case TAPE_MODE_AFF:
				if (m_tape.aff(10) < 0) {
					m_tape_run_flag = false;
					is_send_event = true;
				}
				else {
					::Sleep(10);
				}
				break;
			}

			if (is_usb_task == true) {
				int ret = libusb_submit_transfer(m_tape_transfer);
				if (ret < 0) {
					m_usb_error = true;
					return;
				}
				while (!user_data.completed && m_tape_run_flag == true) {
					if (libusb_handle_events_completed(NULL, &user_data.completed) < 0) {
						if (user_data.completed == 2) {
							m_usb_error = true;
						}
						return;
					}
				}
				if (m_tape_run_flag == false) {
					break;
				}
			}
			if (m_tape_run_flag == true && m_tape_mode == TAPE_MODE_REC) {
				if (m_tape.write_usb_data_to_tape(m_tape_transfer->buffer, m_tape_transfer->actual_length) < 0) {
					m_tape_run_flag = false;
					is_send_event = true;
				}
			}

			if ((GetTickCount64() - prev_time) > 90) {
				prev_time = GetTickCount64();
				m_event_callback(EVENT_UPDATE_SCREEN);
			}
		}

		m_sensor_state &= ~(TAPE_RUNNING);
		m_tape_mode = TAPE_MODE_STOP;
		// simulate mechanical transition
		::Sleep(500);

		if (is_send_event == true) {
//			::OutputDebugStringA("Stop event");
			send_sensor();
			send_response(PC_REQUEST, DataRecorder::COM_STOP);
		}
		m_event_callback(EVENT_UPDATE_SCREEN);
	}

	enum pc_response_t {
		PC_SENSOR_CHANGE,
		PC_STATUS_CHANGE,
		PC_REQUEST,
		PC_TAPE_SAMPLE_RATE_CHANGE,
	};

	enum tape_sample_rate_t {
		TAPE_SAMPLE_48K = 0,
		TAPE_SAMPLE_44K = 1,
		TAPE_SAMPLE_44K_ALT = 2,
		TAPE_SAMPLE_32K = 3,
	};

	struct pc_response_arg {
		pc_response_t type;
		uint8_t response;
	};

	struct send_response_thread_param_t {
		DataRecorder* obj;
		pc_response_t type;
		uint8_t response;
	};

	struct usb_callback_user_data_t {
		DataRecorder* recorder;
		int completed;
	};

	void send_response_thread(pc_response_t type, uint8_t response)
	{
		struct libusb_transfer* response_transfer;

		if (m_usb_error) {
			return;
		}

		int actual_length = 0;
		uint8_t tmp[2];
		usb_callback_user_data_t user_data;

		user_data.completed = 0;
		user_data.recorder = this;

		tmp[0] = type;
		tmp[1] = response;

//		char tmp_str[256];
//		snprintf(tmp_str, sizeof(tmp_str), "Resp: Type:%d  Data:%d\n", type, response);
//		::OutputDebugStringA(tmp_str);

		response_transfer = libusb_alloc_transfer(0);
		if (response_transfer == NULL) {
			return;
		}

		libusb_fill_bulk_transfer(response_transfer, m_usb_handle, OUT_RESPONSE_EP, tmp,
			2, m_usb_callback, &user_data, USB_TIMEOUT_MS);
		int ret = libusb_submit_transfer(response_transfer);
		if (ret < 0) {
			libusb_free_transfer(response_transfer);
			m_usb_error = true;
			return;
		}
		while (!user_data.completed && m_command_receive_run_flag == true) {
			if (libusb_handle_events_completed(NULL, &user_data.completed) < 0) {
				if (user_data.completed == 2) {
					m_usb_error == true;
				}
				return;
			}
		}
		libusb_free_transfer(response_transfer);
	}

	void send_response(pc_response_t type, uint8_t response)
	{
		std::thread send_response([this, type, response]() {this->send_response_thread(type, response); });
		// in a rude way..
		send_response.detach();
	}

	void send_sensor(void)
	{
		uint8_t sensor;

		sensor = get_sensor();
//		char tmp[256];
//		snprintf(tmp, sizeof(tmp), "Sensor: %d\n", sensor);
//		::OutputDebugStringA(tmp);
		send_response(PC_SENSOR_CHANGE, 0x80 | sensor);
	}

	void start_command_receive_thread(void)
	{
		std::thread receive_thread([this]() {this->command_receive_thread(); });
		receive_thread.swap(m_command_receive_thread);
	}

	void command_receive_thread(void) {
		uint8_t trans_data[512];
		int actual_length = 0;
		ULONGLONG prev_time = 0;

		m_command_receive_transfer = libusb_alloc_transfer(0);

		if (m_command_receive_transfer == NULL) {
			::OutputDebugStringA("command transfer allocation failed.\n");
			return;
		}

		while (m_command_receive_run_flag) {
			usb_callback_user_data_t user_data;

			user_data.completed = 0;
			user_data.recorder = this;

			libusb_fill_bulk_transfer(m_command_receive_transfer, m_usb_handle, IN_COMMAND_EP, trans_data,
				sizeof(trans_data), m_usb_callback, &user_data, 0);
			int ret = libusb_submit_transfer(m_command_receive_transfer);
			if (ret < 0) {
				libusb_free_transfer(m_command_receive_transfer);
				m_usb_error = true;
				return;
			}

			while (!user_data.completed && m_command_receive_run_flag == true) {
				if (libusb_handle_events_completed(NULL, &user_data.completed) < 0) {
					if (user_data.completed == 2) {
						m_usb_error = true;
					}
					libusb_free_transfer(m_command_receive_transfer);
					return;
				}
			}
			// check if canceled
			if (m_command_receive_run_flag == false) {
				libusb_free_transfer(m_command_receive_transfer);
				return;
			}
//			char tmp[512];
//			snprintf(tmp, sizeof(tmp), "Command received: %x (%d)\n", trans_data[0], m_command_receive_transfer->actual_length);
//			::OutputDebugStringA(tmp);

			{
				std::lock_guard<std::mutex> lock(m_command_lock);

				m_command_queue.push(trans_data[0]);
			}
			m_command_cond.notify_one();
		}
		libusb_free_transfer(m_command_receive_transfer);
	}

	void start_command_sender_thread(void)
	{
		std::thread sender_thread([this]() {this->command_sender_thread(); });
		sender_thread.swap(m_command_sender_thread);
	}

	void command_sender_thread(void)
	{
		while (m_command_sender_run_flag) {
			{
				uint8_t command;
				std::unique_lock<std::mutex> a_lock(m_command_lock);
				m_command_cond.wait(a_lock, [this] {return (m_command_queue.size() > 0 || m_command_sender_run_flag == false); });
				// command lock is aquired here
				while (m_command_queue.size() > 0) {
					command = m_command_queue.front();
					m_command_queue.pop();
					a_lock.unlock();

					bool is_respond_immediately;
					is_respond_immediately = process_command(command);

					if (is_respond_immediately == true) {
						send_response(PC_REQUEST, command);
					}
					a_lock.lock();
				}
			}
		}
	}

	void set_usb_sample_rate() {
		uint32_t sample_rate_val;
		tape_sample_rate_t real_usb_sample_rate;
		int usb_sample_rate;

		sample_rate_val = get_tape_sample_rate();
		if (sample_rate_val == 44100 || sample_rate_val == 22050) {
			usb_sample_rate = 44100;
			if (m_use_alt_44k == true) {
				real_usb_sample_rate = TAPE_SAMPLE_44K_ALT;
			}
			else {
				real_usb_sample_rate = TAPE_SAMPLE_44K;
			}
		}
		else if (sample_rate_val == 32000 || sample_rate_val == 16000) {
			usb_sample_rate = 32000;
			real_usb_sample_rate = TAPE_SAMPLE_32K;
		}
		else {
			usb_sample_rate = 48000;
			real_usb_sample_rate = TAPE_SAMPLE_48K;
		}

		m_tape.set_usb_sample_rate(usb_sample_rate);

		send_response(PC_TAPE_SAMPLE_RATE_CHANGE, (uint8_t)real_usb_sample_rate);
	}

	static void __stdcall usb_callback(struct libusb_transfer* xfr) {
		static int recv_count = 0;
		usb_callback_user_data_t* user_data = (usb_callback_user_data_t*)xfr->user_data;
//		char temp[256];

		switch (xfr->status) {
		case LIBUSB_TRANSFER_COMPLETED:
			break;
		case LIBUSB_TRANSFER_ERROR:
			user_data->recorder->m_event_callback(EVENT_USB_ERROR);
			break;
		case LIBUSB_TRANSFER_TIMED_OUT:
//			snprintf(temp, sizeof(temp), "USB: transfer %d timed out\n", xfr->endpoint);
//			::OutputDebugStringA(temp);
			user_data->recorder->m_event_callback(EVENT_USB_ERROR);
			break;
		case LIBUSB_TRANSFER_OVERFLOW:
//			::OutputDebugStringA("USB: transfer overflow\n");
			user_data->recorder->m_event_callback(EVENT_USB_ERROR);
			break;
		case LIBUSB_TRANSFER_CANCELLED:
//			::OutputDebugStringA("USB: transfer canceled.\n");
			break;
		case LIBUSB_TRANSFER_NO_DEVICE:
//			::OutputDebugStringA("Disconnected\n");
			user_data->recorder->m_event_callback(EVENT_USB_DISCONNECTED);
			user_data->completed = 2;
			return;
		default:
			break;
		}

		user_data->completed = 1;
	}


	static constexpr int READ_CHUNK_SIZE = 64;
	static constexpr int  USB_TIMEOUT_MS = 2000;

	static constexpr uint8_t OUT_RESPONSE_EP = 0x01;
	static constexpr uint8_t IN_COMMAND_EP = 0x81;
	static constexpr uint8_t IN_TAPE_EP = 0x86;
	static constexpr uint8_t OUT_TAPE_EP = 0x04;

	void(__stdcall* m_usb_callback)(struct libusb_transfer* xfr);
	libusb_device_handle* m_usb_handle;
	struct libusb_transfer* m_tape_transfer;
	uint8_t m_sensor_state;
	tape_mode_t m_tape_mode;
	TapFile m_tape;
	bool m_tape_run_flag;
	std::thread m_usb_thread;

	bool m_command_receive_run_flag;
	struct libusb_transfer* m_command_receive_transfer;
	std::thread m_command_receive_thread;

	void(*m_event_callback)(uint8_t);
	bool m_is_send_event;
	bool m_use_alt_44k;

	std::mutex m_command_lock;
	std::queue<uint8_t> m_command_queue;
	std::condition_variable m_command_cond;
	bool m_command_sender_run_flag;
	std::thread m_command_sender_thread;
	bool m_usb_error;
};
