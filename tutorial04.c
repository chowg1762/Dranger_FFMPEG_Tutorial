
// tutorial04.c
// A pedagogical video player that will stream through every video frame as fast as it can.
//
// This tutorial was written by Stephen Dranger (dranger@gmail.com).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard, 
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
//
// Use the Makefile to build all examples.
//
// Run using
// tutorial02 myvideofile.mpg
//
// to play the video stream on your screen.


#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL.h>
#include <SDL_thread.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>
#include <assert.h>
#include <math.h>

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

typedef struct PacketQueue{
  AVPacketList *first_pkt, *last_pkt;
  int nb_packets;
  int size;
  SDL_mutex *mutex;
  SDL_cond *cond;
} PacketQueue;

typedef struct VideoPicture{
	SDL_Renderer *renderer;
    SDL_Texture *texture;

    int width, height;
    int allocated;

    Uint8 *yPlane, *uPlane, *vPlane;
    size_t yPlaneSz, uvPlaneSz;
    int uvPitch;
}VideoPicture;


typedef struct VideoState{
	AVFormatContext	*pFormatCtx;
	int				videoStream, audioStream;
	AVStream 		*audio_st;
	AVCodecContext	*audio_ctx;
	PacketQueue 	audioq;
	uint8_t			audio_buf[(MAX_AUDIO_FRAME_SIZE*3)/2];
	unsigned int 	audio_buf_size;
	unsigned int 	audio_buf_index;
	AVFrame 		audio_frame;
	AVPacket		audio_pkt;
	uint8_t			*audio_pkt_data;
	int 			audio_pkt_size;
	
	AVStream		*video_st;
	AVCodecContext	*video_ctx;
	PacketQueue		videoq;
	struct SwsContext *sws_ctx;

	VideoPicture	pictq[VIDEO_PICTURE_QUEUE_SIZE];
	int				pictq_size, pictq_rindex, pictq_windex;
	SDL_mutex		*pictq_mutex;
	SDL_cond		*pictq_cond;

	SDL_Thread		*parse_tid;
	SDL_Thread		*video_tid;

	char 			filename[1024];
	int				quit;
}VideoState;

PacketQueue 	audioq;
int 			quit = 0;

SDL_Window 		*screen;
SDL_mutex 		*screen_mutex;
VideoState 		*global_video_state;

void packet_queue_init(PacketQueue *q) {
  fprintf(stderr, "packet_queue_init\n");
  memset(q, 0, sizeof(PacketQueue));
  q->mutex = SDL_CreateMutex();
  q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
	//enqueue
  fprintf(stderr,"packet_queue_put\n");
  AVPacketList *pkt1;
  if(av_dup_packet(pkt) < 0) {
    fprintf(stderr,"av_dup_packet error\n");
    return -1;
  }
  pkt1 = av_malloc(sizeof(AVPacketList));
  if (!pkt1){
     fprintf(stderr,"pkt1 error\n");
    return -1;
    }
  pkt1->pkt = *pkt;
  pkt1->next = NULL;
  
  
  SDL_LockMutex(q->mutex);
  
  if (!q->last_pkt)
    q->first_pkt = pkt1;
  else
    q->last_pkt->next = pkt1;
  q->last_pkt = pkt1;
  q->nb_packets++;
  q->size += pkt1->pkt.size;
  SDL_CondSignal(q->cond);
  
  SDL_UnlockMutex(q->mutex);
  return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
	//dequeue
  printf(stderr, "packet_queue_get\n");
  AVPacketList *pkt1;
  int ret;
  
  SDL_LockMutex(q->mutex);
  
  for(;;) {
    
    if(quit) {
      ret = -1;
      break;
    }

    pkt1 = q->first_pkt;
    if (pkt1) {
      q->first_pkt = pkt1->next;
      if (!q->first_pkt)
    	q->last_pkt = NULL;
      q->nb_packets--;
      q->size -= pkt1->pkt.size;
      *pkt = pkt1->pkt;
      av_free(pkt1);
      ret = 1;
      break;
    } else if (!block) {
      ret = 0;
      break;
    } else {
      SDL_CondWait(q->cond, q->mutex);
    }
  }
  SDL_UnlockMutex(q->mutex);
  return ret;
}

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size) {
 fprintf(stderr, "audio_decode_frame\n");
  int len1, data_size = 0;
  AVPacket *pkt = &is -> audio_pkt;

  for(;;) {
    while(is->audio_pkt_size > 0) {
      int got_frame = 0;
      len1 = avcodec_decode_audio4(is->audio_ctx, &is->audio_frame, &got_frame, pkt);
      if(len1 < 0) {
    /* if error, skip frame */
    is->audio_pkt_size = 0;
    break;
      }
      
      data_size = 0;
      if(got_frame) {
    data_size = av_samples_get_buffer_size(NULL, 
                           is->audio_ctx->channels,
                           is->audio_frame.nb_samples,
                           is->audio_ctx->sample_fmt,
                           1);
    assert(data_size <= buf_size);
    memcpy(audio_buf, is->audio_frame.data[0], data_size);
      }
      is->audio_pkt_data += len1;
      is->audio_pkt_size -= len1;
      if(data_size <= 0) {
    	/* No data yet, get more frames */
	    continue;
	   }
      	/* We have data, return it and come back for more later */
      	fprintf(stderr, "return data size L190\n");
      	return data_size;
    }
    if(pkt->data)
      	av_free_packet(pkt);

    if(is->quit) {
      	fprintf(stderr, "return quit L197\n");
      	return -1; 
    }

    if(packet_queue_get(&is->audioq, pkt, 1) < 0) {
        fprintf(stderr, "packet_queue_get error\n");
      	return -1;
    }

    is->audio_pkt_data = pkt->data;
    is->audio_pkt_size = pkt->size;
  }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
  fprintf(stderr, "audio_callback\n");
  VideoState *is = (VideoState *)userdata;
  int len1, audio_size;

  while(len > 0) {
    if(is->audio_buf_index >= is->audio_buf_size) {
        /* We have already sent all our data; get more */
        audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf));
        
        if(audio_size < 0) {
            /* If error, output silence */
            is->audio_buf_size = 1024; // arbitrary?
            memset(is->audio_buf, 0, is->audio_buf_size);
        } else {
           is->audio_buf_size = audio_size;
        }
        is->audio_buf_index = 0;
    }

    len1 = is->audio_buf_size - is->audio_buf_index;
    if(len1 > len)
      len1 = len;
    memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
    len -= len1;
    stream += len1;
    is->audio_buf_index += len1;
  }
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque){
	fprintf(stderr, "sdl_refresh_timer_cb\n");
	SDL_Event event;
	event.type = FF_REFRESH_EVENT;
	event.user.data1 = opaque;
	SDL_PushEvent(&event);
	return 0;
}

static void schedule_refresh(VideoState *is, int delay){
	fprintf(stderr, "schedule_refresh\n");
	SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}


void video_display(VideoState *is){
	fprintf(stderr, "video_display\n");
	VideoPicture *vp;
	float aspect_ratio;
	int i;

	vp = &is->pictq[is->pictq_rindex];
	if(vp->texture){
		if(is->video_st->codec->sample_aspect_ratio.num == 0){
			aspect_ratio = 0;
		} else{
			aspect_ratio = av_q2d(is->video_st->codec->sample_aspect_ratio)*
				is->video_st->codec->width/ is->video_st->codec->height;
		}

		if(aspect_ratio <= 0.0) {
	      aspect_ratio = (float)is->video_st->codec->width /
			(float)is->video_st->codec->height;
	    }

        SDL_UpdateYUVTexture(
                vp->texture,
                NULL,
                vp->yPlane,
                is->video_st->codec->width,
                vp->uPlane,
                vp->uvPitch,
                vp->vPlane,
                vp->uvPitch
            );
        SDL_LockMutex(screen_mutex);
        SDL_RenderClear(vp->renderer);
        SDL_RenderCopy(vp->renderer, vp->texture, NULL, NULL);
        SDL_RenderPresent(vp->renderer);
        SDL_UnlockMutex(screen_mutex);
	}
}

void video_refresh_timer(void *userdata){
	fprintf(stderr, "video_refresh_timer\n");
	VideoState *is = (VideoState *)userdata;
	VideoPicture *vp;

	if(is->video_st){
		if(is->pictq_size == 0){
			schedule_refresh(is,1);
		} else {
			vp = &is -> pictq[is->pictq_rindex];

			schedule_refresh(is, 40);

			video_display(is);

			if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE){
				is->pictq_rindex = 0;
			}

			SDL_LockMutex(is->pictq_mutex);
			is->pictq_size--;
			SDL_CondSignal(is->pictq_cond);
			SDL_UnlockMutex(is->pictq_mutex);
		}
	} else{
		schedule_refresh(is,100);
	}

}


void alloc_picture(void *userdata){
	fprintf(stderr, "alloc_picture\n");
	VideoState *is = (VideoState *) userdata;
	VideoPicture *vp;

	vp = &is->pictq[is->pictq_windex];
	
	if(vp->texture){
		SDL_DestroyTexture(vp->texture);
		SDL_RenderClear(vp->renderer);
		SDL_DestroyRenderer(vp->renderer);
	}

	SDL_LockMutex(screen_mutex);
	
	vp->renderer = SDL_CreateRenderer(screen, -1, 0);
    if (!vp->renderer) {
        fprintf(stderr, "SDL: could not create renderer - exiting\n");
        exit(1);
    }

	vp->texture = SDL_CreateTexture(
        vp->renderer,
        SDL_PIXELFORMAT_YV12,
        SDL_TEXTUREACCESS_STREAMING,
        is->video_st->codec->width,
        is->video_st->codec->height
    );

    SDL_UnlockMutex(screen_mutex);
    vp->width = is->video_st->codec->width;
    vp->height = is->video_st->codec->height;
    vp->allocated = 1;
}


int queue_picture(VideoState *is, AVFrame *pFrame){
	fprintf(stderr, "queue_picture\n");
	VideoPicture *vp;
	int dst_pix_fmt;
	AVPicture pict;

	SDL_LockMutex(is->pictq_mutex);
	while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit){
		SDL_CondWait(is->pictq_cond, is->pictq_mutex);
	}
	SDL_UnlockMutex(is->pictq_mutex);

	if(is->quit){
		return -1;
	}

	vp = &is->pictq[is->pictq_windex];

	if(!vp->texture ||
		vp->width != is->video_st->codec->width ||
		vp->height!= is->video_st->codec->height){
		SDL_Event event;

		vp->allocated=0;
		alloc_picture(is);
		if(is->quit){
			return -1;
		}
	}

	if(vp->texture){
		 // set up YV12 pixel array (12 bits per pixel)
	    vp->yPlaneSz = is->video_st->codec->width * is->video_st->codec->height;
	    vp->uvPlaneSz = is->video_st->codec->width * is->video_st->codec->height/ 4;
	    vp->yPlane = (Uint8*)malloc(vp->yPlaneSz);
	    vp->uPlane = (Uint8*)malloc(vp->uvPlaneSz);
	    vp->vPlane = (Uint8*)malloc(vp->uvPlaneSz);

	    if (!vp->yPlane || !vp->uPlane || !vp->vPlane) {
	        fprintf(stderr, "Could not allocate pixel buffers - exiting\n");
	        exit(1);
	    }

	    vp->uvPitch = is->video_st->codec->width / 2;

	    pict.data[0] = vp->yPlane;
        pict.data[1] = vp->uPlane;
        pict.data[2] = vp->vPlane;
        pict.linesize[0] = is->video_st->codec->width;
        pict.linesize[1] = vp->uvPitch;
        pict.linesize[2] = vp->uvPitch;

        // Convert the image into YUV format that SDL uses
        sws_scale(is->sws_ctx, (uint8_t const * const *) pFrame->data,
                pFrame->linesize, 0, is->video_st->codec->height, pict.data,
                pict.linesize);

      
       
       if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE){
       	is->pictq_windex = 0;
       }
       SDL_LockMutex(is->pictq_mutex);
       is->pictq_size++;
       SDL_UnlockMutex(is->pictq_mutex);


	    free(vp->yPlane);
	    free(vp->uPlane);
	    free(vp->vPlane);

	}

	return 0;
}


int video_thread(void *arg){
	fprintf(stderr, "video_thread\n");
	VideoState *is = (VideoState *) arg;
	AVPacket pktl, *packet = &pktl;
	int frameFinished;
	AVFrame *pFrame;

	pFrame = av_frame_alloc();

	for(;;){
		if(packet_queue_get(&is->videoq, packet, 1)<0){
			break;
		}

		avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet);

		if(frameFinished){
			if(queue_picture(is, pFrame)<0){
				break;
			}
		}
		av_free_packet(packet);

	}
	//av_free(pFrame);
	av_frame_free(&pFrame);
	return 0;
}


int stream_component_open(VideoState *is, int stream_index){
	fprintf(stderr, "stream_component_open\n");
	AVFormatContext *pFormatCtx = is->pFormatCtx;
	AVCodecContext *codecCtx;
	AVCodec *codec;
	SDL_AudioSpec wanted_spec, spec;

	if(stream_index < 0 || stream_index >= pFormatCtx -> nb_streams){
		return -1;
	}

	codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codec->codec_id);
	if(!codec){
		fprintf(stderr, "Unsupported codec ! \n");
		return -1;
	}

	codecCtx = avcodec_alloc_context3(codec);
	if(avcodec_copy_context(codecCtx, pFormatCtx->streams[stream_index]->codec)!=0){
		fprintf(stderr, "Couldn't copy codec context");
		return -1;
	}

	if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO){
		wanted_spec.freq = codecCtx->sample_rate;
	    wanted_spec.format = AUDIO_S16SYS;
	    wanted_spec.channels = codecCtx->channels;
	    wanted_spec.silence = 0;
	    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	    wanted_spec.callback = audio_callback;
	    wanted_spec.userdata = is;

	    if(SDL_OpenAudio(&wanted_spec, &spec)<0){
	    	fprintf(stderr, "SDL_OpenAudio : %s\n", SDL_GetError());
	    	return -1;
	    }
	}

	if(avcodec_open2(codecCtx, codec, NULL) < 0){
		fprintf(stderr, "Unsupported codec!\n");
		return -1;
	}

	switch(codecCtx->codec_type){
		case AVMEDIA_TYPE_AUDIO : 
			is->audioStream = stream_index;
			is->audio_st 	= pFormatCtx->streams[stream_index];
			is->audio_ctx	= codecCtx;
			is->audio_buf_size=0;
			is->audio_buf_index=0;

			memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
			packet_queue_init(&is->audioq);
			SDL_PauseAudio(0);
			break;
		case AVMEDIA_TYPE_VIDEO :
			is->videoStream = stream_index;
			is->video_st 	= pFormatCtx->streams[stream_index];
			is->video_ctx 	= codecCtx;

			packet_queue_init(&is->videoq);
			is->video_tid = SDL_CreateThread(video_thread, "thread2", is);
			is->sws_ctx   = sws_getContext(is->video_st->codec->width, 
										   is->video_st->codec->height,
				            is->video_st->codec->pix_fmt, is->video_st->codec->width, 
				            is->video_st->codec->height,
				            AV_PIX_FMT_YUV420P,
				            SWS_BILINEAR,
				            NULL,
				            NULL,
				            NULL);
			break;
		default :
			break;
	}
}	


int decode_thread(void *arg){
	
	VideoState *is = (VideoState *) arg;
	fprintf(stderr, "%s decode_thread\n", is->filename);
	AVFormatContext *pFormatCtx=NULL;
	AVPacket pktl, *packet = &pktl;

	int video_index = -1;
	int audio_index = -1;
	int i;

	is->videoStream = -1;
	is->audioStream = -1;

	global_video_state = is;

	fprintf(stderr, "Below is error\n");
	if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL)!=0){
		fprintf(stderr, "decode_thread2\n");
		return -1;
	}

	fprintf(stderr, "decode_thread5\n");
	is->pFormatCtx = pFormatCtx;

	if(avformat_find_stream_info(pFormatCtx, NULL)<0){
		fprintf(stderr, "decode_thread3\n");
		return -1;
	}


	av_dump_format(pFormatCtx, 0 , is->filename, 0);

	  // Find the first video stream
   
    for (i = 0; i < pFormatCtx->nb_streams; i++){
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && video_index<0) {
            video_index = i;
            
        }

        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO  && audio_index<0){
        	audio_index = i;
        	
        }
    }

    if (audio_index >=0){
      	stream_component_open(is, audio_index);
    }

    if (video_index >= 0){
        stream_component_open(is, video_index);
    }
    if(is->videoStream < 0 || is->audioStream<0){
    	fprintf(stderr, "%s : could not open codecs \n ", is->filename);
    	goto fail;
    }

    for(;;){
    	if(is->quit){
    		break;
    	}

    	if(is->audioq.size > MAX_AUDIOQ_SIZE || 
    	   is->videoq.size > MAX_VIDEOQ_SIZE){
    		SDL_Delay(10);
    		continue;
    	}

    	if(av_read_frame(is->pFormatCtx, packet)<0){
    		if((is->pFormatCtx->pb->error)==0){
    			SDL_Delay(100);
    			continue;
    		} else{
    			break;
    		}
    	}

    	if(packet->stream_index == is->videoStream){
    		packet_queue_put(&is->videoq, packet);
    	} else if(packet->stream_index == is->audioStream){
    		packet_queue_put(&is->audioq, packet);
    	} else{
    		av_free_packet(packet);
    	}
    }

    while(!is->quit){
    	SDL_Delay(100);
    }

    fail:
    	if(1){
    		fprintf(stderr, "decode_thread5\n");
    		SDL_Event event;
    		event.type = FF_QUIT_EVENT;
    		event.user.data1 = is;
    		SDL_PushEvent(&event);
    	}
fprintf(stderr, "decode_thread6\n");
    return 0;
}



int main(int argc, char *argv[]) {
    fprintf(stderr, "main\n");
    
    SDL_Event 		event;
	VideoState		*is;

	is = av_mallocz(sizeof(VideoState));

    if (argc < 2) {
        fprintf(stderr, "Usage: test <file>\n");
        exit(1);
    }
    // Register all formats and codecs
    av_register_all();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }


    // Make a screen to put our video
    screen = SDL_CreateWindow(
        "FFmpeg Tutorial",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        640,
        480,
        0
    );

    if (!screen) {
        fprintf(stderr, "SDL: could not create window - exiting\n");
        exit(1);
    }

    screen_mutex = SDL_CreateMutex();
    av_strlcpy(is->filename, argv[1], sizeof(is->filename));

    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond	= SDL_CreateCond();

    schedule_refresh(is, 40);

    is->parse_tid = SDL_CreateThread(decode_thread, "thread1", is);

   	fprintf(stderr, "here\n");

    if(!is->parse_tid){

    	av_free(is);

    	return -1;
    }

  
   for(;;){
   		SDL_WaitEvent(&event);
   		switch(event.type){
   			case FF_QUIT_EVENT:
   			case SDL_QUIT:
   				is->quit = 1; 
   				exit(1);
   				SDL_Quit();
   				return 0;
   				break; 
   			case FF_REFRESH_EVENT:
   				video_refresh_timer(event.user.data1);
   				break;
   			default : 
   			 	break;
   		}
   }

    return 0;
}