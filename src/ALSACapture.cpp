/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** ALSACapture.cpp
** 
** V4L2 RTSP streamer                                                                 
**                                                                                    
** ALSA capture overide of V4l2Capture
**                                                                                    
** -------------------------------------------------------------------------*/

#ifdef HAVE_ALSA

#include "ALSACapture.h"


ALSACapture* ALSACapture::createNew(const ALSACaptureParameters & params) 
{ 
	ALSACapture* capture = new ALSACapture(params);
	if (capture) 
	{
		if (capture->getFd() == -1) 
		{
			delete capture;
			capture = NULL;
		}
	}
	return capture; 
}

ALSACapture::~ALSACapture()
{
	this->close();
}

void ALSACapture::close()
{
	if (m_pcm != NULL)
	{
		snd_pcm_close (m_pcm);
		m_pcm = NULL;
	}
}
	
ALSACapture::ALSACapture(const ALSACaptureParameters & params) : m_pcm(NULL), m_bufferSize(0), m_periodSize(0), m_params(params)
{
	LOG(NOTICE) << "Open ALSA device: \"" << params.m_devName << "\"";
	
	snd_pcm_hw_params_t *hw_params = NULL;
	int err = 0;
	
	// open PCM device
	if ((err = snd_pcm_open (&m_pcm, m_params.m_devName.c_str(), SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		LOG(ERROR) << "cannot open audio device: " << m_params.m_devName << " error:" <<  snd_strerror (err);
	}
				
	// configure hw_params
	else if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) {
		LOG(ERROR) << "cannot allocate hardware parameter structure device: " << m_params.m_devName << " error:" <<  snd_strerror (err);
		this->close();
	}
	else if ((err = snd_pcm_hw_params_any (m_pcm, hw_params)) < 0) {
		LOG(ERROR) << "cannot initialize hardware parameter structure device: " << m_params.m_devName << " error:" <<  snd_strerror (err);
		this->close();
	}			
	else if ((err = snd_pcm_hw_params_set_access (m_pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		LOG(ERROR) << "cannot set access type device: " << m_params.m_devName << " error:" <<  snd_strerror (err);
		this->close();
	}
	else if (this->configureFormat(hw_params) < 0) {
		this->close();
	}
	else if ((err = snd_pcm_hw_params_set_rate_near (m_pcm, hw_params, &m_params.m_sampleRate, 0)) < 0) {
		LOG(ERROR) << "cannot set sample rate device: " << m_params.m_devName << " error:" <<  snd_strerror (err);
		this->close();
	}
	else if ((err = snd_pcm_hw_params_set_channels (m_pcm, hw_params, m_params.m_channels)) < 0) {
		LOG(ERROR) << "cannot set channel count device: " << m_params.m_devName << " error:" <<  snd_strerror (err);
		this->close();
	}
	else if ((err = snd_pcm_hw_params (m_pcm, hw_params)) < 0) {
		LOG(ERROR) << "cannot set parameters device: " << m_params.m_devName << " error:" <<  snd_strerror (err);
		this->close();
	}
	
	// get buffer size
	else if ((err = snd_pcm_get_params(m_pcm, &m_bufferSize, &m_periodSize)) < 0) {
		LOG(ERROR) << "cannot get parameters device: " << m_params.m_devName << " error:" <<  snd_strerror (err);
		this->close();
	}
	
	// start capture
	else if ((err = snd_pcm_prepare (m_pcm)) < 0) {
		LOG(ERROR) << "cannot prepare audio interface for use device: " << m_params.m_devName << " error:" <<  snd_strerror (err);
		this->close();
	}			
	else if ((err = snd_pcm_start (m_pcm)) < 0) {
		LOG(ERROR) << "cannot start audio interface for use device: " << m_params.m_devName << " error:" <<  snd_strerror (err);
		this->close();
	}			
	
	//SR - DS
	m_10MSBufferCache.resize((snd_pcm_format_physical_width(m_fmt) / 8) * m_params.m_channels * (m_params.m_sampleRate / 100));

	LOG(NOTICE) << "ALSA device: \"" << m_params.m_devName << "\" buffer_size:" << m_bufferSize << " period_size:" << m_periodSize << " rate:" << m_params.m_sampleRate;
}
			
int ALSACapture::configureFormat(snd_pcm_hw_params_t *hw_params) {
	
	// try to set format, widht, height
	std::list<snd_pcm_format_t>::iterator it;
	for (it = m_params.m_formatList.begin(); it != m_params.m_formatList.end(); ++it) {
		snd_pcm_format_t format = *it;
		int err = snd_pcm_hw_params_set_format (m_pcm, hw_params, format);
		if (err < 0) {
			LOG(NOTICE) << "cannot set sample format device: " << m_params.m_devName << " to:" << format << " error:" <<  snd_strerror (err);
		} else {
			LOG(NOTICE) << "set sample format device: " << m_params.m_devName << " to:" << format << " ok";
			m_fmt = format;
			return 0;
		}		
	}
	return -1;
}



template<typename DataType, uint8_t ChannelCount>
struct TAudioData
{
	struct AudioData
	{
		DataType Data[ChannelCount];
	};
	

	void ConvertToINT16SingleChannel(const void *InData, int16_t *OutData, int32_t FrameCount)
	{
		int32_t ShiftCount = (sizeof(DataType) - sizeof(int16_t)) * 8;
		
		const AudioData * ptr = (AudioData*)InData;

		for (int32_t Iter = 0; Iter < FrameCount; Iter++)
		{
			OutData[Iter] = (int16_t)(ptr[Iter].Data[0] >> ShiftCount);
		}
	}
};

int16_t swap_int16(int16_t val)
{
	return (val << 8) | ((val >> 8) & 0xFF);
}


size_t ALSACapture::read(char* buffer, size_t bufferSize)
{
	size_t size = 0;
	int fmt_phys_width_bytes = 0;

	const int32_t DesiredFrameCount = (m_params.m_sampleRate / 100);

	assert((int32_t)(bufferSize / sizeof(int16_t)) == DesiredFrameCount);
	assert((int32_t)(m_10MSBufferCache.size() / (snd_pcm_format_physical_width(m_fmt) / 8) / m_params.m_channels) == DesiredFrameCount);

	if (m_pcm != 0)
	{	
		fmt_phys_width_bytes = snd_pcm_format_physical_width(m_fmt) / 8;
		
		snd_pcm_sframes_t ret = snd_pcm_readi (m_pcm, m_10MSBufferCache.data(), DesiredFrameCount);

		LOG(DEBUG) << "ALSA buffer in_size:" << m_periodSize*fmt_phys_width_bytes << " read_size:" << ret;
		if (ret > 0) 
		{
			assert(ret <= DesiredFrameCount);

			int16_t *obuffer = (int16_t*) buffer;
			size = ret * sizeof(int16_t);

			TAudioData<int32_t, 4> DataIn;
			DataIn.ConvertToINT16SingleChannel(m_10MSBufferCache.data(), obuffer, ret);
			
			// swap if capture in not in network order
			if (!snd_pcm_format_big_endian(m_fmt)) 
			{
				for(int32_t Iter = 0; Iter < ret; Iter++)
				{
					obuffer[Iter] = swap_int16(obuffer[Iter]);
				}
			}
		}
	}

	return size;
}
		
int ALSACapture::getFd()
{
	unsigned int nbfs = 1;
	struct pollfd pfds[nbfs]; 
	pfds[0].fd = -1;
	
	if (m_pcm != 0)
	{
		int count = snd_pcm_poll_descriptors_count (m_pcm);
		int err = snd_pcm_poll_descriptors(m_pcm, pfds, count);
		if (err < 0) {
			fprintf (stderr, "cannot snd_pcm_poll_descriptors (%s)\n", snd_strerror (err));
		}
	}
	return pfds[0].fd;
}
		
#endif


