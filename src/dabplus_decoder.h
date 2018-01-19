/*
    DABlin - capital DAB experience
    Copyright (C) 2015-2017 Stefan Pöschel

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DABPLUS_DECODER_H_
#define DABPLUS_DECODER_H_

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdexcept>
#include <sstream>
#include <string>

#if !(defined(DABLIN_AAC_FAAD2) ^ defined(DABLIN_AAC_FDKAAC))
#error "You must select a AAC decoder by defining either DABLIN_AAC_FAAD2 or DABLIN_AAC_FDKAAC!"
#endif

#ifdef DABLIN_AAC_FAAD2
#include <neaacdec.h>
#endif

#ifdef DABLIN_AAC_FDKAAC
#include <fdk-aac/aacdecoder_lib.h>
#endif

extern "C" {
#include <fec.h>
}

#include "subchannel_sink.h"
#include "tools.h"


struct SuperframeFormat {
	bool dac_rate;
	bool sbr_flag;
	bool aac_channel_mode;
	bool ps_flag;
	int mpeg_surround_config;

	int GetCoreSrIndex() {
		return dac_rate ? (sbr_flag ? 6 : 3) : (sbr_flag ? 8 : 5);	// 24/48/16/32 kHz
	}
	int GetCoreChConfig() {
		return aac_channel_mode ? 2 : 1;
	}
	int GetExtensionSrIndex() {
		return dac_rate ? 3 : 5;	// 48/32 kHz
	}
};


// --- adts-header---- //cyang add
typedef struct adts_fixed_header 
{  
	unsigned int syncword:12;//ͬ����0xfff��˵��һ��ADTS֡�Ŀ�ʼ
	unsigned char ID:1;//ID�Ƚ����,��׼�ĵ�������ô˵�ġ�MPEG identifier, set to ��1��. See ISO/IEC 11172-3��,����д0��,Ҳ����
	unsigned char layer:2;//һ������Ϊ0
	unsigned char protection_absent:1;//�Ƿ�����У��
	unsigned char profile:2;//��ʾʹ���ĸ������AAC����01 Low Complexity(LC)--- AACLC
	unsigned char sampling_frequency_index:4;//��ʾʹ�õĲ������±�0x3 48k ,0x4 44.1k, 0x5 32k
	unsigned char private_bit:1;//һ������Ϊ0
	unsigned char channel_configuration:3;// ��ʾ������
	unsigned char original_copy:1;//һ������Ϊ0
	unsigned char home:1;//һ������Ϊ0
}adts_fixed;

typedef struct adts_variable_header
{  
	unsigned char copyright_identification_bit:1;//һ������Ϊ0
	unsigned char copyright_identification_start:1;//һ������Ϊ0
	unsigned int frame_length:13;// һ��ADTS֡�ĳ��Ȱ���ADTSͷ��raw data block
	unsigned int adts_buffer_fullness:11;// 0x7FF ˵�������ʿɱ������
	unsigned char number_of_raw_data_blocks_in_frame:2;//��ʾADTS֡����number_of_raw_data_blocks_in_frame + 1��AACԭʼ֡.
}adts_variable;


// --- RSDecoder -----------------------------------------------------------------
class RSDecoder {
private:
	void *rs_handle;
	uint8_t rs_packet[120];
	int corr_pos[10];
public:
	RSDecoder();
	~RSDecoder();

	void DecodeSuperframe(uint8_t *sf, size_t sf_len);
};



// --- AACDecoder -----------------------------------------------------------------
class AACDecoder {
protected:
	SubchannelSinkObserver* observer;
	uint8_t asc[7];
	size_t asc_len;
	uint8_t adts_header[7];  //cyang add
public:
	AACDecoder(std::string decoder_name, SubchannelSinkObserver* observer, SuperframeFormat sf_format);
	virtual ~AACDecoder() {}

	virtual void DecodeFrame(uint8_t *data, size_t len) = 0;
};


#ifdef DABLIN_AAC_FAAD2
// --- AACDecoderFAAD2 -----------------------------------------------------------------
class AACDecoderFAAD2 : public AACDecoder {
private:
	NeAACDecHandle handle;
	NeAACDecFrameInfo dec_frameinfo;
public:
	AACDecoderFAAD2(SubchannelSinkObserver* observer, SuperframeFormat sf_format);
	~AACDecoderFAAD2();

	void DecodeFrame(uint8_t *data, size_t len);
};
#endif


#ifdef DABLIN_AAC_FDKAAC
// --- AACDecoderFDKAAC -----------------------------------------------------------------
class AACDecoderFDKAAC : public AACDecoder {
private:
	HANDLE_AACDECODER handle;
	uint8_t *output_frame;
	size_t output_frame_len;
public:
	AACDecoderFDKAAC(SubchannelSinkObserver* observer, SuperframeFormat sf_format);
	~AACDecoderFDKAAC();

	void DecodeFrame(uint8_t *data, size_t len);
};
#endif


// --- SuperframeFilter -----------------------------------------------------------------
class SuperframeFilter : public SubchannelSink {
private:
	RSDecoder rs_dec;
	AACDecoder *aac_dec;

	size_t frame_len;
	int frame_count;
	int sync_frames;

	uint8_t *sf_raw;
	uint8_t *sf;
	size_t sf_len;

	bool sf_format_set;
	uint8_t sf_format_raw;
	SuperframeFormat sf_format;

	int num_aus;
	int au_start[6+1]; // +1 for end of last AU

	bool CheckSync();
	void ProcessFormat();
	void CheckForPAD(const uint8_t *data, size_t len);
	void ResetPAD();
public:
	SuperframeFilter(SubchannelSinkObserver* observer);
	~SuperframeFilter();

	void Feed(const uint8_t *data, size_t len);
};



#endif /* DABPLUS_DECODER_H_ */
