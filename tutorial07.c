
// tutorial05.c
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

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

#define DEFAULT_AV_SYNC_TYPE AV_SYNC_VIDEO_MASTER

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

    double pts;
}VideoPicture;


typedef struct VideoState{
	AVFormatContext	*pFormatCtx;
	int				videoStream, audioStream;

	int 			av_sync_type;
	double			external_clock;
	int64_t 		external_clock_time;
	int 			seek_req;
	int 			seek_flags;
	int64_t			seek_pos;

	double 			audio_clock;
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
	double 			audio_hw_buf_size;
	double 			audio_diff_cum;
	double 			audio_diff_avg_coef;
	double			audio_diff_threshold;
	int 			audio_diff_avg_count;
	double			frame_timer;
	double			frame_last_pts;
	double  		frame_last_delay;
	double 			video_clock;
	double 			video_current_pts;
	int64_t			video_current_pts_time;
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

enum{
	AV_SYNC_AUDIO_MASTER,
	AV_SYNC_VIDEO_MASTER,
	AV_SYNC_EXTERNAL_MASTER,
};

int 			quit = 0;

SDL_Window 		*screen;
SDL_mutex 		*screen_mutex;
VideoState 		*global_video_state;
AVPacket  		flush_pkt;

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

  if(pkt != &flush_pkt && av_dup_packet(pkt)<0){
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

static void packet_queue_flush(PacketQueue *q){
	fprintf(stderr, "packet_queue_flush\n" );
	AVPacketList *pkt, *pktl;

	SDL_LockMutex(q->mutex);
	for(pkt = q->first_pkt; pkt != NULL; pkt = pktl){
		pktl = pkt->next;
		av_free_packet(&pkt->pkt);
		av_free(&pkt);
	}
	q->last_pkt = NULL;
	q->first_pkt = NULL;
	q->nb_packets = 0;
	q->size = 0;
	SDL_UnlockMutex(q->mutex);
}

double get_audio_clock(VideoState *is){
	fprintf(stderr, "get_audio_clock\n" );
	double 	pts;
	int 	hw_buf_size, bytes_per_sec, n;

	pts = is->audio_clock;
	hw_buf_size = is->audio_buf_size - is->audio_buf_index;
	bytes_per_sec = 0;
	n = is->audio_ctx->channels*2;

	if(is->audio_st){
		bytes_per_sec = is->audio_ctx->sample_rate * n;
	} 

	if(bytes_per_sec){
		pts -= (double)hw_buf_size / bytes_per_sec;
	}

	return pts;
}

double get_video_clock(VideoState *is){
	fprintf(stderr, "get_video_clock\n" );
	double delta;
	delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
	return is->video_current_pts + delta;
}

double get_external_clock(VideoState *is){
	fprintf(stderr, "get_external_clock\n" );
	return av_gettime() / 1000000.0;
}

double get_master_clock(VideoState *is){
	fprintf(stderr, "get_master_clock\n" );
	if(is->av_sync_type == AV_SYNC_VIDEO_MASTER){
		return get_video_clock(is);
	} else if(is->av_sync_type == AV_SYNC_AUDIO_MASTER){
		return get_audio_clock(is);
	} else{
		return get_external_clock(is);
	}

}

int synchronize_audio(VideoState *is, short *samples, int samples_size, double pts){
	fprintf(stderr, "synchronize_audio\n" );
	int n;
	double ref_clock;

	n = 2 * is->audio_ctx->channels;

	if(is->av_sync_type != AV_SYNC_AUDIO_MASTER){
		double diff, avg_diff;
		int    wanted_size, min_size, max_size;

		ref_clock = get_master_clock(is);
		diff = get_audio_clock(is) - ref_clock;

		if(diff < AV_NOSYNC_THRESHOLD){
			is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
		
			if(is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB){
				is->audio_diff_avg_count++;
			} else{
				avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
				if(fabs(avg_diff) >= is->audio_diff_threshold){
					wanted_size = samples_size + ((int)(diff*is->audio_ctx->sample_rate)*n);
					min_size    = samples_size * ((100-SAMPLE_CORRECTION_PERCENT_MAX)/100);
					max_size   = samples_size * ((100+SAMPLE_CORRECTION_PERCENT_MAX)/100);

					if(wanted_size < min_size){
						wanted_size = min_size;
					} else if(wanted_size > max_size){
						wanted_size = max_size;
					}

					if(wanted_size < samples_size){
						samples_size = wanted_size;
					} else if(wanted_size> samples_size){
						uint8_t *samples_end, *q;
						int nb;

						nb = (samples_size - wanted_size);
						samples_end = (uint8_t *)samples + samples_size - n;
						q = samples_end + n;
						
						while(nb>0){
							memcpy(q, samples_end, n);
							q+=n;
							nb-=n;
						}

						samples_size = wanted_size;
					}
				}
			}
		} else{
			is->audio_diff_avg_count = 0;
			is->audio_diff_cum = 0;
		}
	}

	return samples_size;
}

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size, double *pts_ptr) {
 fprintf(stderr, "audio_decode_frame\n");
  int len1, data_size = 0;
  AVPacket *pkt = &is -> audio_pkt;
  double pts;
  int n;

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

	   pts = is->audio_clock;
	   *pts_ptr = pts;
	   n = 2 * is->audio_ctx->channels;
	   is->audio_clock += (double) data_size/(double)(n*is->audio_ctx->sample_rate);
      	/* We have data, return it and come back for more later */
   
      	return data_size;
    }
    if(pkt->data)
      	av_free_packet(pkt);

    if(is->quit) {
      	return -1; 
    }

    if(packet_queue_get(&is->audioq, pkt, 1) < 0) {
      	return -1;
    }

    if(pkt->data == flush_pkt.data){
    	avcodec_flush_buffers(is->audio_ctx);
    	continue;
    }

    is->audio_pkt_data = pkt->data;
    is->audio_pkt_size = pkt->size;

    if(pkt->pts != AV_NOPTS_VALUE){
    	is->audio_clock = av_q2d(is->audio_st->time_base)*pkt->pts;
    }
  }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
 fprintf(stderr, "audio_callback\n" );
  VideoState *is = (VideoState *)userdata;
  int len1, audio_size;
  double pts;

  while(len > 0) {
    if(is->audio_buf_index >= is->audio_buf_size) {
        /* We have already sent all our data; get more */
        audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf), &pts);
        
        if(audio_size < 0) {
            /* If error, output silence */
            is->audio_buf_size = 1024; // arbitrary?
            memset(is->audio_buf, 0, is->audio_buf_size);
        } else {
        	audio_size = synchronize_audio(is, (int16_t *)is->audio_buf,
        			audio_size, pts);
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
	double actual_delay, delay, sync_threshold, ref_clock, diff;

	if(is->video_st){
		if(is->pictq_size == 0){
			schedule_refresh(is,1);
		} else {
			vp = &is->pictq[is->pictq_rindex];

			is->video_current_pts = vp->pts;
			is->video_current_pts_time = av_gettime();
			delay = vp->pts - is->frame_last_pts;
			if(delay<=0 || delay >= 1.0){
				delay = is->frame_last_delay;
			}

			is->frame_last_delay = delay;
			is->frame_last_pts	 = vp->pts;

			if(is->av_sync_type != AV_SYNC_VIDEO_MASTER){
				ref_clock = get_master_clock(is);
				diff = vp->pts - ref_clock;

				sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
				
				if(fabs(diff)<AV_NOSYNC_THRESHOLD){
					if(diff <= -sync_threshold){
						delay = 0;
					}else if(diff >= sync_threshold){
						delay = 2 * delay;
					}
				}
			}

			is->frame_timer += delay;
			actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
			if(actual_delay<0.010){
				actual_delay = 0.010;
			}


			schedule_refresh(is, (int)(actual_delay*1000+0.5));

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


int queue_picture(VideoState *is, AVFrame *pFrame, double pts){
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
		vp->pts = pts;
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

double synchronize_video(VideoState *is, AVFrame *src_frame, double pts){
	fprintf(stderr, "synchronize_video\n" );
	double frame_delay;

	if(pts != 0){
		is->video_clock = pts;
	}else{

		pts = is->video_clock;
	}

	frame_delay = av_q2d(is->video_ctx->time_base);

	frame_delay += src_frame->repeat_pict * (frame_delay*0.5);
	is->video_clock += frame_delay;

	return pts;
}


int video_thread(void *arg){
	fprintf(stderr, "video_thread\n");
	VideoState *is = (VideoState *) arg;
	AVPacket pktl, *packet = &pktl;
	int frameFinished;
	AVFrame *pFrame;
	double pts;

	pFrame = av_frame_alloc();

	for(;;){
		if(packet_queue_get(&is->videoq, packet, 1)<0){
			break;
		}



		pts = 0;

		avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet);

		if((pts = av_frame_get_best_effort_timestamp(pFrame))== AV_NOPTS_VALUE){
			pts = av_frame_get_best_effort_timestamp(pFrame);
		}else{
			pts = 0;
		}

		pts*=av_q2d(is->video_st->time_base);

		if(frameFinished){
			pts = synchronize_video(is, pFrame, pts);
			if(queue_picture(is, pFrame,pts)<0){
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
	AVCodecContext *codecCtx = NULL;
	AVCodec *codec = NULL;
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
	    is->audio_hw_buf_size = spec.size;
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

			is->frame_timer = (double)av_gettime() /1000000.0;
			is->frame_last_delay = 40e-3;
			is->video_current_pts_time = av_gettime();

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

	if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL)!=0){	
		return -1;
	}

	is->pFormatCtx = pFormatCtx;

	if(avformat_find_stream_info(pFormatCtx, NULL)<0){
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
    	if(is->seek_req){
    		int stream_index = -1; 
    		int64_t seek_target = is->seek_pos;

    		if(is->videoStream >=0) stream_index = is->videoStream;
    		else if(is->audioStream>=0) stream_index = is->audioStream;

    		if(stream_index>=0){
    			seek_target = av_rescale_q(seek_target, AV_TIME_BASE_Q, 
    				pFormatCtx->streams[stream_index]->time_base);
    		}
    		if(av_seek_frame(is->pFormatCtx, stream_index, seek_target, is->seek_flags)<0){
    			fprintf(stderr, "%sL error while seeking\n", is->pFormatCtx->filename);
			}else{
				if(is->audioStream >= 0){
					packet_queue_flush(&is->audioq);
					packet_queue_put(&is->audioq, &flush_pkt);
				}

				if(is->videoStream >= 0){
					packet_queue_flush(&is->videoq);
					packet_queue_put(&is->videoq, &flush_pkt);
				}
			}

			is->seek_req = 0;			
		
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
    		SDL_Event event;
    		event.type = FF_QUIT_EVENT;
    		event.user.data1 = is;
    		SDL_PushEvent(&event);
    	}
    return 0;
}

void stream_seek(VideoState *is, int64_t pos, int rel){
	fprintf(stderr, "stream seek\n");
	if(!is->seek_req){
		is->seek_pos = pos;
		is->seek_flags = rel < 0? AVSEEK_FLAG_BACKWARD : 0;
		is -> seek_req = 1;
	}
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

    is->av_sync_type = DEFAULT_AV_SYNC_TYPE;
    is->parse_tid = SDL_CreateThread(decode_thread, "thread1", is);

    if(!is->parse_tid){

    	av_free(is);

    	return -1;
    }

    av_init_packet(&flush_pkt);
    flush_pkt.data = "FLUSH";
  
   for(;;){
   		double incr, pos;
   		SDL_WaitEvent(&event);
   		switch(event.type){
   			case SDL_KEYDOWN :
   				switch(event.key.keysym.sym){
   					case SDLK_RIGHT:
	   					incr = 10.0;
	   					goto do_seek;
	   				case SDLK_LEFT :
	   					incr = -10.0;
	   					goto do_seek;
	   				case SDLK_DOWN:
	   					incr = -60.0;
	   					goto do_seek;
	   				case SDLK_UP :
	   					incr = 60.0;
	   					goto do_seek;

	   				do_seek : 
	   				if(global_video_state){
	   					pos = get_master_clock(global_video_state);
	   					pos+=incr;
	   					stream_seek(global_video_state, (int64_t)(pos*AV_TIME_BASE), incr);
	   				}

	   				break;
	   				default : 
	   				break;

   				}

   				break;
   			case FF_QUIT_EVENT:
   			case SDL_QUIT:
   				is->quit = 1; 
   				exit(1);

   				SDL_CondSignal(is->audioq.cond);
   				SDL_CondSignal(is->videoq.cond);
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