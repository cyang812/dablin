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

#include "dabplus_decoder.h"


// --- SuperframeFilter -----------------------------------------------------------------
SuperframeFilter::SuperframeFilter(SubchannelSinkObserver* observer) : SubchannelSink(observer) {
	aac_dec = NULL;

	frame_len = 0;
	frame_count = 0;
	sync_frames = 0;

	sf_raw = NULL;
	sf = NULL;
	sf_len = 0;

	sf_format_set = false;
	sf_format_raw = 0;

	num_aus = 0;
}

SuperframeFilter::~SuperframeFilter() {
	delete[] sf_raw;
	delete[] sf;
	delete aac_dec;
}

void SuperframeFilter::Feed(const uint8_t *data, size_t len) {
	// check frame len
	if(frame_len) {
		if(frame_len != len) {
			fprintf(stderr, "SuperframeFilter: different frame len %zu (should be: %zu) - frame ignored!\n", len, frame_len);
			return;
		}
	} else {
		if(len < 10) {
			fprintf(stderr, "SuperframeFilter: frame len %zu too short - frame ignored!\n", len);
			return;
		}
		if((5 * len) % 120) {
			fprintf(stderr, "SuperframeFilter: resulting Superframe len of len %zu not divisible by 120 - frame ignored!\n", len);
			return;
		}

		frame_len = len;
		sf_len = 5 * frame_len;

		sf_raw = new uint8_t[sf_len];
		sf = new uint8_t[sf_len];
	}

	if(frame_count == 5) {
		// shift previous frames
		for(int i = 0; i < 4; i++)
			memcpy(sf_raw + i * frame_len, sf_raw + (i + 1) * frame_len, frame_len);
	} else {
		frame_count++;
	}

	// copy frame
	memcpy(sf_raw + (frame_count - 1) * frame_len, data, frame_len);

	if(frame_count < 5)
		return;


	// append RS coding on copy
	memcpy(sf, sf_raw, sf_len);
	rs_dec.DecodeSuperframe(sf, sf_len);

	if(!CheckSync()) {
		if(sync_frames == 0)
			fprintf(stderr, "SuperframeFilter: Superframe sync started...\n");
		sync_frames++;
		return;
	}

	if(sync_frames) {
		fprintf(stderr, "SuperframeFilter: Superframe sync succeeded after %d frame(s)\n", sync_frames);
		sync_frames = 0;
		ResetPAD();
	}


	// check announced format
	if(!sf_format_set || sf_format_raw != sf[2]) {
		sf_format_raw = sf[2];
		sf_format_set = true;

		ProcessFormat();
	}

	// decode frames
	for(int i = 0; i < num_aus; i++) {
		uint8_t *au_data = sf + au_start[i];
		size_t au_len = au_start[i+1] - au_start[i];

		uint16_t au_crc_stored = au_data[au_len-2] << 8 | au_data[au_len-1];
		uint16_t au_crc_calced = CalcCRC::CalcCRC_CRC16_CCITT.Calc(au_data, au_len - 2);
		if(au_crc_stored != au_crc_calced) {
			fprintf(stderr, "\x1B[31m" "(AU #%d)" "\x1B[0m" " ", i);
			ResetPAD();
			continue;
		}

		au_len -= 2;
		aac_dec->DecodeFrame(au_data, au_len);
		CheckForPAD(au_data, au_len);
	}

	// ensure getting a complete new Superframe
	frame_count = 0;
}


void SuperframeFilter::CheckForPAD(const uint8_t *data, size_t len) {
	bool present = false;

	// check for PAD (embedded into Data Stream Element)
	if(len >= 3 && (data[0] >> 5) == 4) {
		size_t pad_start = 2;
		size_t pad_len = data[1];
		if(pad_len == 255) {
			pad_len += data[2];
			pad_start++;
		}

		if(pad_len >= 2 && len >= pad_start + pad_len) {
			observer->ProcessPAD(data + pad_start, pad_len - FPAD_LEN, true, data + pad_start + pad_len - FPAD_LEN);
			present = true;
		}
	}

	if(!present)
		ResetPAD();
}

void SuperframeFilter::ResetPAD() {
	// required to reset internal state of PAD parser (in case of omitted CI list)
	uint8_t zero_fpad[FPAD_LEN] = {0x00};
	observer->ProcessPAD(NULL, 0, true, zero_fpad);
}


bool SuperframeFilter::CheckSync() {
	// abort, if au_start is kind of zero (prevent sync on complete zero array)
	if(sf[3] == 0x00 && sf[4] == 0x00)
		return false;

	// TODO: use fire code for error correction

	// try to sync on fire code
	uint16_t crc_stored = sf[0] << 8 | sf[1];
	uint16_t crc_calced = CalcCRC::CalcCRC_FIRE_CODE.Calc(sf + 2, 9);
	if(crc_stored != crc_calced)
		return false;


	// handle format
	sf_format.dac_rate             = sf[2] & 0x40;
	sf_format.sbr_flag             = sf[2] & 0x20;
	sf_format.aac_channel_mode     = sf[2] & 0x10;
	sf_format.ps_flag              = sf[2] & 0x08;
	sf_format.mpeg_surround_config = sf[2] & 0x07;


	// determine number/start of AUs
	num_aus = sf_format.dac_rate ? (sf_format.sbr_flag ? 3 : 6) : (sf_format.sbr_flag ? 2 : 4);

	au_start[0] = sf_format.dac_rate ? (sf_format.sbr_flag ? 6 : 11) : (sf_format.sbr_flag ? 5 : 8);
	au_start[num_aus] = sf_len / 120 * 110;	// pseudo-next AU (w/o RS coding)

	au_start[1] = sf[3] << 4 | sf[4] >> 4;
	if(num_aus >= 3)
		au_start[2] = (sf[4] & 0x0F) << 8 | sf[5];
	if(num_aus >= 4)
		au_start[3] = sf[6] << 4 | sf[7] >> 4;
	if(num_aus == 6) {
		au_start[4] = (sf[7] & 0x0F) << 8 | sf[8];
		au_start[5] = sf[9] << 4 | sf[10] >> 4;
	}

	// simple plausi check for correct order of start offsets
	for(int i = 0; i < num_aus; i++)
		if(au_start[i] >= au_start[i+1])
			return false;

	return true;
}


void SuperframeFilter::ProcessFormat() {
	// output format
	const char *stereo_mode = (sf_format.aac_channel_mode || sf_format.ps_flag) ? "Stereo" : "Mono";
	const char *surround_mode;

	switch(sf_format.mpeg_surround_config) {
	case 0:
		surround_mode = NULL;
		break;
	case 1:
		surround_mode = "Surround 5.1";
		break;
	case 2:
		surround_mode = "Surround 7.1";
		break;
	default:
		surround_mode = "Surround (unknown)";
		break;
	}

	int bitrate = sf_len / 120 * 8;

	std::stringstream ss;
	ss << (sf_format.sbr_flag ? (sf_format.ps_flag ? "HE-AAC v2" : "HE-AAC") : "AAC-LC") << ", ";
	ss << (sf_format.dac_rate ? 48 : 32) << " kHz ";
	if(surround_mode)
		ss << surround_mode << " (" << stereo_mode << " core) ";
	else
		ss << stereo_mode << " ";
	ss << "@ " << bitrate << " kBit/s";
	observer->FormatChange(ss.str());

	if(aac_dec)
		delete aac_dec;
#ifdef DABLIN_AAC_FAAD2
	aac_dec = new AACDecoderFAAD2(observer, sf_format);
#endif
#ifdef DABLIN_AAC_FDKAAC
	aac_dec = new AACDecoderFDKAAC(observer, sf_format);
#endif
}


// --- RSDecoder -----------------------------------------------------------------
RSDecoder::RSDecoder() {
	rs_handle = init_rs_char(8, 0x11D, 0, 1, 10, 135);
	if(!rs_handle)
		throw std::runtime_error("RSDecoder: error while init_rs_char");
}

RSDecoder::~RSDecoder() {
	free_rs_char(rs_handle);
}

void RSDecoder::DecodeSuperframe(uint8_t *sf, size_t sf_len) {
//	// insert errors for test
//	sf[0] ^= 0xFF;
//	sf[10] ^= 0xFF;
//	sf[20] ^= 0xFF;

	int subch_index = sf_len / 120;
	int total_corr_count = 0;
	bool uncorr_errors = false;

	// process all RS packets
	for(int i = 0; i < subch_index; i++) {
		for(int pos = 0; pos < 120; pos++)
			rs_packet[pos] = sf[pos * subch_index + i];

		// detect errors
		int corr_count = decode_rs_char(rs_handle, rs_packet, corr_pos, 0);
		if(corr_count == -1)
			uncorr_errors = true;
		else
			total_corr_count += corr_count;

		// correct errors
		for(int j = 0; j < corr_count; j++) {

			int pos = corr_pos[j] - 135;
			if(pos < 0)
				continue;

//			fprintf(stderr, "j: %d, pos: %d, sf-index: %d\n", j, pos, pos * subch_index + i);
			sf[pos * subch_index + i] = rs_packet[pos];
		}
	}

	// output statistics if errors present (using ANSI coloring)
	if(total_corr_count || uncorr_errors)
		fprintf(stderr, "\x1B[36m" "(%d%s)" "\x1B[0m" " ", total_corr_count, uncorr_errors ? "+" : "");
}



// --- AACDecoder -----------------------------------------------------------------
AACDecoder::AACDecoder(std::string decoder_name, SubchannelSinkObserver* observer, SuperframeFormat sf_format) {
	fprintf(stderr, "AACDecoder: using decoder '%s'\n", decoder_name.c_str());

	this->observer = observer;

	/* AudioSpecificConfig structure (the only way to select 960 transform here!)
	 *
	 *  00010 = AudioObjectType 2 (AAC LC)
	 *  xxxx  = (core) sample rate index
	 *  xxxx  = (core) channel config
	 *  100   = GASpecificConfig with 960 transform
	 *
	 * SBR: explicit signaling (backwards-compatible), adding:
	 *  01010110111 = sync extension for SBR
	 *  00101       = AudioObjectType 5 (SBR)
	 *  1           = SBR present flag
	 *  xxxx        = extension sample rate index
	 *
	 * PS:  explicit signaling (backwards-compatible), adding:
	 *  10101001000 = sync extension for PS
	 *  1           = PS present flag
	 *
	 * Note:
	 * libfaad2 does not support non backwards-compatible PS signaling (AOT 29);
	 * it detects PS only by implicit signaling.
	 */

	// AAC LC
	asc_len = 0;
	asc[asc_len++] = 0b00010 << 3 | sf_format.GetCoreSrIndex() >> 1;
	asc[asc_len++] = (sf_format.GetCoreSrIndex() & 0x01) << 7 | sf_format.GetCoreChConfig() << 3 | 0b100;

	if(sf_format.sbr_flag) {
		// add SBR
		asc[asc_len++] = 0x56;
		asc[asc_len++] = 0xE5;
		asc[asc_len++] = 0x80 | (sf_format.GetExtensionSrIndex() << 3);

		if(sf_format.ps_flag) {
			// add PS
			asc[asc_len - 1] |= 0x05;
			asc[asc_len++] = 0x48;
			asc[asc_len++] = 0x80;
		}
	}

	adts_fixed *fixed;
	adts_variable *variable;
	memset(adts_header,0,7);
	fixed = (adts_fixed *)malloc(sizeof(adts_fixed) *1);
	variable = (adts_variable *)malloc(sizeof(adts_variable) *1);

	fixed->syncword = 0xfff;
	fixed->ID = 0x0;   // ID��MPEG Version: 0 for MPEG-4��1 for MPEG-2
	fixed->layer = 0x0;  //always '00'
	fixed->protection_absent = 0x1;  //no crc
	fixed->profile = 0x1;   //AAC LC
	fixed->sampling_frequency_index = sf_format.GetExtensionSrIndex();//sample;
	fixed->private_bit = 0x0;
	fixed->channel_configuration = sf_format.GetCoreChConfig();//channel; 
	fixed->original_copy = 0x0;
	fixed->home = 0x0;

	variable->copyright_identification_bit = 0x0;
	variable->copyright_identification_start = 0x0;
	variable->frame_length = 0x80;//len;
	variable->adts_buffer_fullness = 0x7ff;
	variable->number_of_raw_data_blocks_in_frame = 0x0;
	
	adts_header[0] = (fixed->syncword>>4);  //0xff;
	adts_header[1] = (((fixed->syncword & 0xF)<<4)|(fixed->ID<<3)|(fixed->layer<<1)|(fixed->protection_absent));
	adts_header[2] = ((fixed->profile<<6)|(fixed->sampling_frequency_index<<2)|(fixed->private_bit<<1)|((fixed->channel_configuration>>2)&0x1));
	adts_header[3] = (((fixed->channel_configuration&0x3)<<6)|(fixed->original_copy<<5)|(fixed->home<<4)|(variable->copyright_identification_bit<<3)|(variable->copyright_identification_start<<2)|((variable->frame_length>>11)&0x3));
	adts_header[4] = ((variable->frame_length>>3)&0xff);
	adts_header[5] = (((variable->frame_length&0x7)<<5)|((variable->adts_buffer_fullness>>6)&0x1f));
	adts_header[6] = (((variable->adts_buffer_fullness&0x3F)<<2)|(variable->number_of_raw_data_blocks_in_frame));
}


#ifdef DABLIN_AAC_FAAD2
// --- AACDecoderFAAD2 -----------------------------------------------------------------
AACDecoderFAAD2::AACDecoderFAAD2(SubchannelSinkObserver* observer, SuperframeFormat sf_format) : AACDecoder("FAAD2", observer, sf_format) {
	// ensure features
	unsigned long cap = NeAACDecGetCapabilities();
	if(!(cap & LC_DEC_CAP))
		throw std::runtime_error("AACDecoderFAAD2: no LC decoding support!");

	handle = NeAACDecOpen();
	if(!handle)
		throw std::runtime_error("AACDecoderFAAD2: error while NeAACDecOpen");

	// set general config
	NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(handle);
	if(!config)
		throw std::runtime_error("AACDecoderFAAD2: error while NeAACDecGetCurrentConfiguration");

	config->outputFormat = FAAD_FMT_FLOAT;
	config->dontUpSampleImplicitSBR = 0;

	if(NeAACDecSetConfiguration(handle, config) != 1)
		throw std::runtime_error("AACDecoderFAAD2: error while NeAACDecSetConfiguration");

	// init decoder
	unsigned long output_sr;
	unsigned char output_ch;
	long int init_result = NeAACDecInit2(handle, asc, asc_len, &output_sr, &output_ch);
	if(init_result != 0)
		throw std::runtime_error("AACDecoderFAAD2: error while NeAACDecInit2: " + std::string(NeAACDecGetErrorMessage(-init_result)));

	observer->StartAudio(output_sr, output_ch, true);
}

AACDecoderFAAD2::~AACDecoderFAAD2() {
	NeAACDecClose(handle);
}

void AACDecoderFAAD2::DecodeFrame(uint8_t *data, size_t len) {
	// decode audio
	uint8_t* output_frame = (uint8_t*) NeAACDecDecode(handle, &dec_frameinfo, data, len);
	if(dec_frameinfo.error)
		fprintf(stderr, "\x1B[35m" "(AAC)" "\x1B[0m" " ");

	// abort, if no output at all
	if(dec_frameinfo.bytesconsumed == 0 && dec_frameinfo.samples == 0)
		return;

	if(dec_frameinfo.bytesconsumed != len)
		throw std::runtime_error("AACDecoderFAAD2: NeAACDecDecode did not consume all bytes");

	observer->PutAudio(output_frame, dec_frameinfo.samples * 4);
}
#endif



#ifdef DABLIN_AAC_FDKAAC
// --- AACDecoderFDKAAC -----------------------------------------------------------------
AACDecoderFDKAAC::AACDecoderFDKAAC(SubchannelSinkObserver* observer, SuperframeFormat sf_format) : AACDecoder("FDK-AAC", observer, sf_format) {
//	handle = aacDecoder_Open(TT_MP4_RAW, 1);
	handle = aacDecoder_Open(TT_MP4_ADTS, 1);
	if(!handle)
		throw std::runtime_error("AACDecoderFDKAAC: error while aacDecoder_Open");

	int channels = sf_format.aac_channel_mode || sf_format.ps_flag ? 2 : 1;
	AAC_DECODER_ERROR init_result;

	/* Restrict output channel count to actual input channel count.
	 *
	 * Just using the parameter value -1 (no up-/downmix) does not work, as with
	 * SBR and Mono the lib assumes possibly present PS and then outputs Stereo!
	 *
	 * Note:
	 * Older lib versions use a combined parameter for the output channel count.
	 * As the headers of these didn't define the version, branch accordingly.
	 */
#if !defined(AACDECODER_LIB_VL0) && !defined(AACDECODER_LIB_VL1) && !defined(AACDECODER_LIB_VL2)
	init_result = aacDecoder_SetParam(handle, AAC_PCM_OUTPUT_CHANNELS, channels);
	if(init_result != AAC_DEC_OK)
		throw std::runtime_error("AACDecoderFDKAAC: error while setting parameter AAC_PCM_OUTPUT_CHANNELS: " + std::to_string(init_result));
#else
	init_result = aacDecoder_SetParam(handle, AAC_PCM_MIN_OUTPUT_CHANNELS, channels);
	if(init_result != AAC_DEC_OK)
		throw std::runtime_error("AACDecoderFDKAAC: error while setting parameter AAC_PCM_MIN_OUTPUT_CHANNELS: " + std::to_string(init_result));
	init_result = aacDecoder_SetParam(handle, AAC_PCM_MAX_OUTPUT_CHANNELS, channels);
	if(init_result != AAC_DEC_OK)
		throw std::runtime_error("AACDecoderFDKAAC: error while setting parameter AAC_PCM_MAX_OUTPUT_CHANNELS: " + std::to_string(init_result));
#endif

	uint8_t* asc_array[1] {asc};
	const unsigned int asc_sizeof_array[1] {(unsigned int) asc_len};
	fwrite(asc, 7 , 1, stdout);    //cyang add
	init_result = aacDecoder_ConfigRaw(handle, asc_array, asc_sizeof_array);
	if(init_result != AAC_DEC_OK)
		throw std::runtime_error("AACDecoderFDKAAC: error while aacDecoder_ConfigRaw: " + std::to_string(init_result));

	output_frame_len = 960 * 2 * channels * (sf_format.sbr_flag ? 2 : 1);
	output_frame = new uint8_t[output_frame_len];

	observer->StartAudio(sf_format.dac_rate ? 48000 : 32000, channels, false);
}

AACDecoderFDKAAC::~AACDecoderFDKAAC() {
	aacDecoder_Close(handle);
	delete[] output_frame;
}

void AACDecoderFDKAAC::DecodeFrame(uint8_t *data, size_t len) {
	uint8_t* input_buffer[1] {data};
//	const unsigned int input_buffer_size[1] {(unsigned int) len};
	unsigned int input_buffer_size[1] {(unsigned int) len};
	unsigned int bytes_valid = len;

	//fprintf(stderr, "bytes_valid = %d\n",bytes_valid);

//	uint8_t frame_len[2] ={0x00,0x00};
//	frame_len[0] = (uint8_t)((bytes_valid>>8)&0xff);
//	frame_len[1] = (uint8_t)(bytes_valid&0xff);
//	fprintf(stderr, "frame_len[0] = %#x\n",frame_len[0]);
//	fprintf(stderr, "frame_len[1] = %#x\n",frame_len[1]);

	//delete pad
	uint8_t pad_len = data[1];
	if(data[0] == 0x80)
	{
		data += pad_len + 2;
		bytes_valid -= pad_len + 2;
	}
	
#if 0 //write adts
	uint32_t aac_frame_size = bytes_valid + 7; //len + adts_header_len
	uint32_t adts_buffer_fullness = 0x7ff;
	adts_header[3] = adts_header[3]|(aac_frame_size>>11&0x3);
	adts_header[4] = ((aac_frame_size>>3)&0xff);
	adts_header[5] = (((aac_frame_size&0x7)<<5)|((adts_buffer_fullness>>6)&0x1f));

//	fwrite(adts_header, 7 , 1, stdout);    //cyang add write adts_header 
//	fwrite(data, len, 1, stdout);          //cyang add write frame_data
//	fwrite(frame_len, 2, 1, stdout);       //cyang add write frame_len

	//for adts dec
    uint8_t adts_frame[1000];
    memcpy(adts_frame,adts_header,7);
	memcpy((uint8_t *)&adts_frame[7],data,bytes_valid);
	input_buffer[0] = adts_frame;
	input_buffer_size[0] = bytes_valid + 7;

	fwrite(adts_frame,bytes_valid+7,1,stdout);
#else  //write raw_data_len + raw_data
	uint8_t raw_data_len[4];
	raw_data_len[0] = 0xff;
	raw_data_len[1] = 0xff;
	raw_data_len[2] = (uint8_t)((bytes_valid>>8) & 0xff);
	raw_data_len[3] = (uint8_t)(bytes_valid & 0xff);
	
	fwrite(raw_data_len, 4, 1, stdout);	   //cyang add write frame_len
	fwrite(data, bytes_valid, 1, stdout);  //cyang add write frame_data
#endif

#if 0
	// fill internal input buffer
	AAC_DECODER_ERROR result = aacDecoder_Fill(handle, input_buffer, input_buffer_size, &bytes_valid);
	if(result != AAC_DEC_OK)
		throw std::runtime_error("AACDecoderFDKAAC: error while aacDecoder_Fill: " + std::to_string(result));
	if(bytes_valid)
		throw std::runtime_error("AACDecoderFDKAAC: aacDecoder_Fill did not consume all bytes");

	// decode audio
	result = aacDecoder_DecodeFrame(handle, (short int*) output_frame, output_frame_len / 2, 0);
	if(result != AAC_DEC_OK)
		fprintf(stderr, "\x1B[35m" "(AAC)" "\x1B[0m" " ");
	if(!IS_OUTPUT_VALID(result))
		return;
#endif
	observer->PutAudio(output_frame, output_frame_len);
}
#endif
