# - 1、去除PCM输出，直接输出AAC原始数据

```c
<pcm_output.cpp>
//	if(!audio_mute)  //cyang modify
//		fwrite(data, len, 1, stdout);  //cyang modify 


<dabplus_decoder.cpp>
	uint8_t* asc_array[1] {asc};  //cyang add
	fwrite(asc_array, 7 , 1, stdout);  //cyang add
	fwrite(data, len, 1, stdout); //cyang add


```