/*****************************************************************************
 * video.c: video decoder using ffmpeg library
 *****************************************************************************
 * Copyright (C) 1999-2001 VideoLAN
 * $Id: video.c,v 1.3 2002/11/06 21:48:24 gbazin Exp $
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Gildas Bazin <gbazin@netcourrier.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>                                      /* malloc(), free() */

#include <vlc/vlc.h>
#include <vlc/vout.h>
#include <vlc/aout.h>
#include <vlc/decoder.h>
#include <vlc/input.h>

#include <string.h>

#ifdef HAVE_SYS_TIMES_H
#   include <sys/times.h>
#endif

#include "avcodec.h"                                               /* ffmpeg */

#include "postprocessing/postprocessing.h"

#include "ffmpeg.h"
#include "video.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void ffmpeg_CopyPicture( picture_t *, AVPicture *, vdec_thread_t * );
static void ffmpeg_PostProcPicture( vdec_thread_t *, picture_t * );
static int  ffmpeg_GetFrameBuf( struct AVCodecContext *, int, int, int );

/* FIXME FIXME some of them are wrong */
static int i_ffmpeg_PixFmtToChroma[] =
{
    /* PIX_FMT_ANY = -1, PIX_FMT_YUV420P, 
       PIX_FMT_YUV422,   PIX_FMT_RGB24,   
       PIX_FMT_BGR24,    PIX_FMT_YUV422P, 
       PIX_FMT_YUV444P,  PIX_FMT_YUV410P 
     */
    0,                           VLC_FOURCC('I','4','2','0'),
    VLC_FOURCC('I','4','2','0'), VLC_FOURCC('R','V','2','4'),
    0,                           VLC_FOURCC('Y','4','2','2'),
    VLC_FOURCC('I','4','4','4'), 0
};

/*****************************************************************************
 * Local Functions
 *****************************************************************************/

static inline u32 ffmpeg_PixFmtToChroma( int i_ffmpegchroma )
{
    if( ++i_ffmpegchroma > 7 )
    {
        return( 0 );
    }
    else
    {
        return( i_ffmpeg_PixFmtToChroma[i_ffmpegchroma] );
    }
}

static inline int ffmpeg_FfAspect( int i_width, int i_height, int i_ffaspect )
{
    switch( i_ffaspect )
    {
        case( FF_ASPECT_4_3_625 ):
        case( FF_ASPECT_4_3_525 ):
            return( VOUT_ASPECT_FACTOR * 4 / 3);
        case( FF_ASPECT_16_9_625 ):
        case( FF_ASPECT_16_9_525 ):
            return( VOUT_ASPECT_FACTOR * 16 / 9 );
        case( FF_ASPECT_SQUARE ):
        default:
            return( VOUT_ASPECT_FACTOR * i_width / i_height );
    }
}

/* Check if we have a Vout with good parameters */
static int ffmpeg_CheckVout( vout_thread_t *p_vout,
                             int i_width,
                             int i_height,
                             int i_aspect,
                             int i_chroma )
{
    if( !p_vout )
    {
        return( 0 );
    }
    if( !i_chroma )
    {
        /* we will try to make conversion */
        i_chroma = VLC_FOURCC('I','4','2','0');
    } 

    if( ( p_vout->render.i_width != i_width )||
        ( p_vout->render.i_height != i_height )||
        ( p_vout->render.i_chroma != i_chroma )||
        ( p_vout->render.i_aspect != 
                ffmpeg_FfAspect( i_width, i_height, i_aspect ) ) )
    {
        return( 0 );
    }
    else
    {
        return( 1 );
    }
}

/* Return a Vout */
static vout_thread_t *ffmpeg_CreateVout( vdec_thread_t *p_vdec,
                                         int i_width,
                                         int i_height,
                                         int i_aspect,
                                         int i_chroma )
{
    vout_thread_t *p_vout;

    if( (!i_width)||(!i_height) )
    {
        return( NULL ); /* Can't create a new vout without display size */
    }

    if( !i_chroma )
    {
        /* we make conversion if possible*/
        i_chroma = VLC_FOURCC('I','4','2','0');
        msg_Warn( p_vdec->p_fifo, "Internal chroma conversion (FIXME)");
        /* It's mainly for I410 -> I420 conversion that I've made,
           it's buggy and very slow */
    } 

    i_aspect = ffmpeg_FfAspect( i_width, i_height, i_aspect );

    /* Spawn a video output if there is none. First we look for our children,
     * then we look for any other vout that might be available. */
    p_vout = vlc_object_find( p_vdec->p_fifo, VLC_OBJECT_VOUT,
                                              FIND_CHILD );
    if( !p_vout )
    {
        p_vout = vlc_object_find( p_vdec->p_fifo, VLC_OBJECT_VOUT,
                                                  FIND_ANYWHERE );
    }

    if( p_vout )
    {
        if( !ffmpeg_CheckVout( p_vout, 
                               i_width, i_height, i_aspect,i_chroma ) )
        {
            /* We are not interested in this format, close this vout */
            vlc_object_detach( p_vout );
            vlc_object_release( p_vout );
            vout_DestroyThread( p_vout );
            p_vout = NULL;
        }
        else
        {
            /* This video output is cool! Hijack it. */
            vlc_object_detach( p_vout );
            vlc_object_attach( p_vout, p_vdec->p_fifo );
            vlc_object_release( p_vout );
        }
    }

    if( p_vout == NULL )
    {
        msg_Dbg( p_vdec->p_fifo, "no vout present, spawning one" );

        p_vout = vout_CreateThread( p_vdec->p_fifo,
                                    i_width, i_height,
                                    i_chroma, i_aspect );
    }

    return( p_vout );
}

/* FIXME FIXME FIXME this is a big shit
   does someone want to rewrite this function ? 
   or said to me how write a better thing
   FIXME FIXME FIXME
*/
static void ffmpeg_ConvertPictureI410toI420( picture_t *p_pic,
                                             AVPicture *p_avpicture,
                                             vdec_thread_t   *p_vdec )
{
    u8 *p_src, *p_dst;
    u8 *p_plane[3];
    int i_plane;

    int i_stride, i_lines;
    int i_height, i_width;
    int i_y, i_x;

    i_height = p_vdec->p_context->height;
    i_width  = p_vdec->p_context->width;

    p_dst = p_pic->p[0].p_pixels;
    p_src  = p_avpicture->data[0];

    /* copy first plane */
    for( i_y = 0; i_y < i_height; i_y++ )
    {
        p_vdec->p_fifo->p_vlc->pf_memcpy( p_dst, p_src, i_width);
        p_dst += p_pic->p[0].i_pitch;
        p_src += p_avpicture->linesize[0];
    }

    /* process each plane in a temporary buffer */
    for( i_plane = 1; i_plane < 3; i_plane++ )
    {
        i_stride = p_avpicture->linesize[i_plane];
        i_lines = i_height / 4;

        p_dst = p_plane[i_plane] = malloc( i_lines * i_stride * 2 * 2 );
        p_src  = p_avpicture->data[i_plane];

        /* for each source line */
        for( i_y = 0; i_y < i_lines; i_y++ )
        {
            for( i_x = 0; i_x < i_stride - 1; i_x++ )
            {
                p_dst[2 * i_x    ] = p_src[i_x];
                p_dst[2 * i_x + 1] = ( p_src[i_x] + p_src[i_x + 1]) / 2;

            }
            p_dst[2 * i_stride - 2] = p_src[i_x];
            p_dst[2 * i_stride - 1] = p_src[i_x];

            p_dst += 4 * i_stride; /* process the next even lines */
            p_src += i_stride;
        }
    }

    for( i_plane = 1; i_plane < 3; i_plane++ )
    {
        i_stride = p_avpicture->linesize[i_plane];
        i_lines = i_height / 4;

        p_dst = p_plane[i_plane] + 2*i_stride;
        p_src  = p_plane[i_plane];

        for( i_y = 0; i_y < i_lines - 1; i_y++ )
        {
            for( i_x = 0; i_x <  2 * i_stride ; i_x++ )
            {
                p_dst[i_x] = ( p_src[i_x] + p_src[i_x + 4*i_stride])/2;
            }

            p_dst += 4 * i_stride; /* process the next odd lines */
            p_src += 4 * i_stride;
        }
        /* last line */
        p_vdec->p_fifo->p_vlc->pf_memcpy( p_dst, p_src, 2*i_stride );
    }
    /* copy to p_pic, by block
       if I do pixel per pixel it segfault. It's why I use 
       temporaries buffers */
    for( i_plane = 1; i_plane < 3; i_plane++ )
    {
        int i_size; 
        p_src  = p_plane[i_plane];
        p_dst = p_pic->p[i_plane].p_pixels;

        i_size = __MIN( 2*i_stride, p_pic->p[i_plane].i_pitch);
        for( i_y = 0; i_y < __MIN(p_pic->p[i_plane].i_lines, 2 * i_lines); i_y++ )
        {
            p_vdec->p_fifo->p_vlc->pf_memcpy( p_dst, p_src, i_size );
            p_src += 2 * i_stride;
            p_dst += p_pic->p[i_plane].i_pitch;
        }
        free( p_plane[i_plane] );
    }
}

/*****************************************************************************
 *
 * Functions that initialize, decode and end the decoding process
 *
 * Functions exported for ffmpeg.c
 *   * E_( InitThread_Video )
 *   * E_( DecodeThread )
 *   * E_( EndThread_Video )
 *****************************************************************************/

/*****************************************************************************
 * InitThread: initialize vdec output thread
 *****************************************************************************
 * This function is called from decoder_Run and performs the second step 
 * of the initialization. It returns 0 on success. Note that the thread's 
 * flag are not modified inside this function.
 *
 * ffmpeg codec will be open, some memory allocated. But Vout is not yet
 * open (done after the first decoded frame)
 *****************************************************************************/
int E_( InitThread_Video )( vdec_thread_t *p_vdec )
{
    int i_tmp;

    if( p_vdec->p_fifo->p_demux_data )
    {
        p_vdec->p_format = (BITMAPINFOHEADER *)p_vdec->p_fifo->p_demux_data;
    }
    else
    {
        msg_Warn( p_vdec->p_fifo, "display informations missing" );
    }

    /* ***** Fill p_context with init values ***** */
    p_vdec->p_context->width  = p_vdec->p_format->biWidth;
    p_vdec->p_context->height = p_vdec->p_format->biHeight;
    
    /*  ***** Get configuration of ffmpeg plugin ***** */
#if LIBAVCODEC_BUILD >= 4611
    i_tmp = config_GetInt( p_vdec->p_fifo, "ffmpeg-workaround-bugs" );
    p_vdec->p_context->workaround_bugs  = __MAX( __MIN( i_tmp, 99 ), 0 );

    i_tmp = config_GetInt( p_vdec->p_fifo, "ffmpeg-error-resilience" );
    p_vdec->p_context->error_resilience = __MAX( __MIN( i_tmp, 99 ), -1 );
#endif
#if LIBAVCODEC_BUILD >= 4614
    if( config_GetInt( p_vdec->p_fifo, "grayscale" ) )
    {
        p_vdec->p_context->flags|= CODEC_FLAG_GRAY;
    }
#endif

    p_vdec->b_hurry_up = config_GetInt(p_vdec->p_fifo, "ffmpeg-hurry-up");

    p_vdec->p_lastpic = NULL;
    p_vdec->p_secondlastpic = NULL;
    p_vdec->b_direct_rendering = 0;
#if LIBAVCODEC_BUILD > 4615
    if( (p_vdec->p_codec->capabilities & CODEC_CAP_DR1)
        && (p_vdec->p_context->pix_fmt != PIX_FMT_YUV410P) )
    {
        msg_Dbg( p_vdec->p_fifo, "using direct rendering" );
        p_vdec->b_direct_rendering = 1;
        p_vdec->p_context->flags|= CODEC_FLAG_EMU_EDGE | CODEC_FLAG_DR1; 
        p_vdec->p_context->get_buffer_callback = ffmpeg_GetFrameBuf;
        p_vdec->p_context->opaque = p_vdec;
    }
#endif

    /* ***** Open the codec ***** */
    if( avcodec_open(p_vdec->p_context, p_vdec->p_codec) < 0 )
    {
        msg_Err( p_vdec->p_fifo, "cannot open codec (%s)",
                                 p_vdec->psz_namecodec );
        return( -1 );
    }
    else
    {
        msg_Dbg( p_vdec->p_fifo, "ffmpeg codec (%s) started",
                                 p_vdec->psz_namecodec );
    }

    /* ***** init this codec with special data ***** */
    if( p_vdec->p_format->biSize > sizeof(BITMAPINFOHEADER) )
    {
        AVPicture avpicture;
        int b_gotpicture;
        
        switch( p_vdec->i_codec_id )
        {
            case( CODEC_ID_MPEG4 ):
                avcodec_decode_video( p_vdec->p_context, &avpicture, 
                                      &b_gotpicture,
                                      (void *)&p_vdec->p_format[1],
                                      p_vdec->p_format->biSize
                                        - sizeof(BITMAPINFOHEADER) );
                break;
            default:
                if( p_vdec->p_fifo->i_fourcc == FOURCC_MP4S ||
                    p_vdec->p_fifo->i_fourcc == FOURCC_mp4s ||
                    p_vdec->p_fifo->i_fourcc == FOURCC_M4S2 ||
                    p_vdec->p_fifo->i_fourcc == FOURCC_m4s2 )
                {
                    p_vdec->p_context->extradata_size =
                        p_vdec->p_format->biSize - sizeof(BITMAPINFOHEADER);
                    p_vdec->p_context->extradata =
                        malloc( p_vdec->p_context->extradata_size );
                    memcpy( p_vdec->p_context->extradata,
                            &p_vdec->p_format[1],
                            p_vdec->p_context->extradata_size );
                }

                break;
        }
    }
    
    /* ***** Load post processing ***** */

    /* get overridding settings */
    p_vdec->i_pp_mode = 0;
    if( config_GetInt( p_vdec->p_fifo, "ffmpeg-db-yv" ) )
        p_vdec->i_pp_mode |= PP_DEBLOCK_Y_V;
    if( config_GetInt( p_vdec->p_fifo, "ffmpeg-db-yh" ) )
        p_vdec->i_pp_mode |= PP_DEBLOCK_Y_H;
    if( config_GetInt( p_vdec->p_fifo, "ffmpeg-db-cv" ) )
        p_vdec->i_pp_mode |= PP_DEBLOCK_C_V;
    if( config_GetInt( p_vdec->p_fifo, "ffmpeg-db-ch" ) )
        p_vdec->i_pp_mode |= PP_DEBLOCK_C_H;
    if( config_GetInt( p_vdec->p_fifo, "ffmpeg-dr-y" ) )
        p_vdec->i_pp_mode |= PP_DERING_Y;
    if( config_GetInt( p_vdec->p_fifo, "ffmpeg-dr-c" ) )
        p_vdec->i_pp_mode |= PP_DERING_C;

    if( ( config_GetInt( p_vdec->p_fifo, "ffmpeg-pp-q" ) > 0 )||
        ( config_GetInt( p_vdec->p_fifo, "ffmpeg-pp-auto" )  )||
        ( p_vdec->i_pp_mode != 0 ) )
    {
        /* check if the codec support postproc. */
        switch( p_vdec->i_codec_id )
        {
#if LIBAVCODEC_BUILD > 4608
            case( CODEC_ID_MSMPEG4V1 ):
            case( CODEC_ID_MSMPEG4V2 ):
            case( CODEC_ID_MSMPEG4V3 ):
#else
            case( CODEC_ID_MSMPEG4 ):
#endif
            case( CODEC_ID_MPEG4 ):
            case( CODEC_ID_H263 ):
//            case( CODEC_ID_H263P ): I don't use it up to now
            case( CODEC_ID_H263I ):
                /* Ok we can make postprocessing :)) */
                /* first try to get a postprocess module */
#if LIBAVCODEC_BUILD > 4613
                p_vdec->p_pp = vlc_object_create( p_vdec->p_fifo,
                                                  sizeof( postprocessing_t ) );
                p_vdec->p_pp->psz_object_name = "postprocessing";
                p_vdec->p_pp->p_module = 
                   module_Need( p_vdec->p_pp, "postprocessing", "$ffmpeg-pp" );

                if( !p_vdec->p_pp->p_module )
                {
                    msg_Warn( p_vdec->p_fifo, 
                              "no suitable postprocessing module" );
                    vlc_object_destroy( p_vdec->p_pp );
                    p_vdec->p_pp = NULL;
                    p_vdec->i_pp_mode = 0;
                }
                else
                {
                    /* get mode upon quality */
                    p_vdec->i_pp_mode |= 
                        p_vdec->p_pp->pf_getmode( 
                              config_GetInt( p_vdec->p_fifo, "ffmpeg-pp-q" ),
                              config_GetInt( p_vdec->p_fifo, "ffmpeg-pp-auto" )
                                                );

                    /* allocate table for postprocess */
                    p_vdec->p_context->quant_store = 
                        malloc( sizeof( int ) * ( MBR + 1 ) * ( MBC + 1 ) );
                    p_vdec->p_context->qstride = MBC + 1;
                }
#else
                p_vdec->i_pp_mode = 0;
                msg_Warn( p_vdec->p_fifo, 
                          "post-processing not supported, upgrade ffmpeg" );
#endif
                break;
            default:
                p_vdec->i_pp_mode = 0;
                msg_Warn( p_vdec->p_fifo, 
                          "Post processing unsupported for this codec" );
                break;
        }

    }
//    memset( &p_vdec->statistic, 0, sizeof( statistic_t ) );

    return( 0 );
}

/*****************************************************************************
 * DecodeThread: Called for decode one frame
 *****************************************************************************
 * We have to get a frame stored in a pes, give it to ffmpeg decoder and send
 * the image to the output.
 *****************************************************************************/
void  E_( DecodeThread_Video )( vdec_thread_t *p_vdec )
{
    pes_packet_t    *p_pes;
    int     i_frame_size;
    int     i_status;
    int     b_drawpicture;
    int     b_gotpicture;
    AVPicture avpicture;                                   /* ffmpeg picture */
    picture_t *p_pic;                                    /* videolan picture */

    /* TODO implement it in a better way */
    /* A good idea could be to decode all I pictures and see for the other */
    if( ( p_vdec->b_hurry_up )&& ( p_vdec->i_frame_late > 4 ) )
    {
#if LIBAVCODEC_BUILD > 4603
        b_drawpicture = 0;
        if( p_vdec->i_frame_late < 8 )
        {
            p_vdec->p_context->hurry_up = 2;
        }
        else
        {
            /* too much late picture, won't decode 
               but break picture until a new I, and for mpeg4 ...*/
            p_vdec->i_frame_late--; /* needed else it will never be decrease */
            input_ExtractPES( p_vdec->p_fifo, NULL );
            return;
        }
#else
        if( p_vdec->i_frame_late < 8 )
        {
            b_drawpicture = 0; /* not really good but .. UPGRADE FFMPEG !! */
        }
        else
        {
            /* too much late picture, won't decode 
               but break picture until a new I, and for mpeg4 ...*/
            p_vdec->i_frame_late--; /* needed else it will never be decrease */
            input_ExtractPES( p_vdec->p_fifo, NULL );
            return;
        }
#endif
    }
    else
    {
        b_drawpicture = 1;
#if LIBAVCODEC_BUILD > 4603
        p_vdec->p_context->hurry_up = 0;
#endif
    }

    do
    {
        input_ExtractPES( p_vdec->p_fifo, &p_pes );
        if( !p_pes )
        {
            p_vdec->p_fifo->b_error = 1;
            return;
        }
        p_vdec->pts = p_pes->i_pts;
        i_frame_size = p_pes->i_pes_size;

        if( i_frame_size > 0 )
        {
            if( p_vdec->i_buffer < i_frame_size + 16 )
            {
                FREE( p_vdec->p_buffer );
                p_vdec->p_buffer = malloc( i_frame_size + 16 );
                p_vdec->i_buffer = i_frame_size + 16;
            }
            
            E_( GetPESData )( p_vdec->p_buffer, p_vdec->i_buffer, p_pes );
        }
        input_DeletePES( p_vdec->p_fifo->p_packets_mgt, p_pes );
    } while( i_frame_size <= 0 );


    i_status = avcodec_decode_video( p_vdec->p_context,
                                     &avpicture,
                                     &b_gotpicture,
                                     p_vdec->p_buffer,
                                     i_frame_size );

    if( i_status < 0 )
    {
        msg_Warn( p_vdec->p_fifo, "cannot decode one frame (%d bytes)",
                                  i_frame_size );
        p_vdec->i_frame_error++;
        return;
    }
    /* Update frame late count*/
    /* I don't make statistic on decoding time */
    if( p_vdec->pts <= mdate()) 
    {
        p_vdec->i_frame_late++;
    }
    else
    {
        p_vdec->i_frame_late = 0;
    }

    if( !b_gotpicture || avpicture.linesize[0] == 0 || !b_drawpicture )
    {
        return;
    }

    if( !p_vdec->b_direct_rendering )
    {
        /* Check our vout */
        if( !ffmpeg_CheckVout( p_vdec->p_vout,
                           p_vdec->p_context->width,
                           p_vdec->p_context->height,
                           p_vdec->p_context->aspect_ratio_info,
                           ffmpeg_PixFmtToChroma(p_vdec->p_context->pix_fmt)) )
        {
            p_vdec->p_vout = ffmpeg_CreateVout( p_vdec,
                           p_vdec->p_context->width,
                           p_vdec->p_context->height,
                           p_vdec->p_context->aspect_ratio_info,
                           ffmpeg_PixFmtToChroma(p_vdec->p_context->pix_fmt) );
            if( !p_vdec->p_vout )
            {
                msg_Err( p_vdec->p_fifo, "cannot create vout" );
                p_vdec->p_fifo->b_error = 1; /* abort */
                return;
            }
        }

        /* Get a new picture */
        while( !(p_pic = vout_CreatePicture( p_vdec->p_vout, 0, 0, 0 ) ) )
        {
            if( p_vdec->p_fifo->b_die || p_vdec->p_fifo->b_error )
            {
                return;
            }
            msleep( VOUT_OUTMEM_SLEEP );
        }

        /* fill p_picture_t from avpicture, do I410->I420 if needed */
        ffmpeg_CopyPicture( p_pic, &avpicture, p_vdec );
    }
    else
    {
#if LIBAVCODEC_BUILD > 4615
        p_pic = (picture_t *)p_vdec->p_context->dr_opaque_frame;
#endif
    }

    /* Do post-processing if requested */
    ffmpeg_PostProcPicture( p_vdec, p_pic );

    /* FIXME correct avi and use i_dts */

    /* Send decoded frame to vout */
    vout_DatePicture( p_vdec->p_vout, p_pic, p_vdec->pts);
    vout_DisplayPicture( p_vdec->p_vout, p_pic );
    
    return;
}

/*****************************************************************************
 * EndThread: thread destruction
 *****************************************************************************
 * This function is called when the thread ends after a sucessful
 * initialization.
 *****************************************************************************/
void E_( EndThread_Video )( vdec_thread_t *p_vdec )
{
    if( p_vdec->p_secondlastpic )
        vout_UnlinkPicture( p_vdec->p_vout, p_vdec->p_secondlastpic );
    if( p_vdec->p_lastpic )
        vout_UnlinkPicture( p_vdec->p_vout, p_vdec->p_lastpic );

    if( p_vdec->p_pp )
    {
        /* release postprocessing module */
        module_Unneed( p_vdec->p_pp, p_vdec->p_pp->p_module );
        vlc_object_destroy( p_vdec->p_pp );
        p_vdec->p_pp = NULL;
    }

    if( p_vdec->p_vout != NULL )
    {
        /* We are about to die. Reattach video output to p_vlc. */
        vlc_object_detach( p_vdec->p_vout );
        vlc_object_attach( p_vdec->p_vout, p_vdec->p_fifo->p_vlc );
    }
}

/*****************************************************************************
 * ffmpeg_CopyPicture: copy a picture from ffmpeg internal buffers to a
 *                     picture_t structure (when not in direct rendering mode).
 *****************************************************************************/
static void ffmpeg_CopyPicture( picture_t *p_pic, AVPicture *p_avpicture,
                                vdec_thread_t *p_vdec )
{
    int i_plane; 
    int i_size;
    int i_line;

    u8  *p_dst;
    u8  *p_src;
    int i_src_stride;
    int i_dst_stride;

    if( ffmpeg_PixFmtToChroma( p_vdec->p_context->pix_fmt ) )
    {
        for( i_plane = 0; i_plane < p_pic->i_planes; i_plane++ )
        {
            p_src  = p_avpicture->data[i_plane];
            p_dst = p_pic->p[i_plane].p_pixels;
            i_src_stride = p_avpicture->linesize[i_plane];
            i_dst_stride = p_pic->p[i_plane].i_pitch;

            i_size = __MIN( i_src_stride, i_dst_stride );
            for( i_line = 0; i_line < p_pic->p[i_plane].i_lines; i_line++ )
            {
                p_vdec->p_fifo->p_vlc->pf_memcpy( p_dst, p_src, i_size );
                p_src += i_src_stride;
                p_dst += i_dst_stride;
            }
        }
    }
    else
    {
        /* we need to convert to I420 */
        switch( p_vdec->p_context->pix_fmt )
        {
#if LIBAVCODEC_BUILD >= 4615
            case( PIX_FMT_YUV410P ):
                ffmpeg_ConvertPictureI410toI420( p_pic, p_avpicture, p_vdec );
                break;
#endif            
            default:
                p_vdec->p_fifo->b_error = 1;
                break;
        }
    }
}

/*****************************************************************************
 * ffmpeg_PostProcPicture: Postprocessing is done here.
 *****************************************************************************/
static void ffmpeg_PostProcPicture( vdec_thread_t *p_vdec, picture_t *p_pic )
{
#if LIBAVCODEC_BUILD > 4313
    if( ( p_vdec->i_pp_mode )&&
        ( ( p_vdec->p_vout->render.i_chroma == 
            VLC_FOURCC( 'I','4','2','0' ) )||
          ( p_vdec->p_vout->render.i_chroma == 
            VLC_FOURCC( 'Y','V','1','2' ) ) ) )
    {
        /* Make postproc */
        p_vdec->p_pp->pf_postprocess( p_pic,
                                      p_vdec->p_context->quant_store, 
                                      p_vdec->p_context->qstride,
                                      p_vdec->i_pp_mode );
    }
#endif
}

/*****************************************************************************
 * ffmpeg_GetFrameBuf: callback used by ffmpeg to get a frame buffer.
 *                     (used for direct rendering)
 *****************************************************************************/
static int ffmpeg_GetFrameBuf( struct AVCodecContext *avctx, int width,
                               int height, int pict_type )
{
#if LIBAVCODEC_BUILD > 4615
    vdec_thread_t *p_vdec = (vdec_thread_t *)avctx->opaque;
    picture_t *p_pic;

    /* Check our vout */
    if( !ffmpeg_CheckVout( p_vdec->p_vout,
                           p_vdec->p_context->width,
                           p_vdec->p_context->height,
                           p_vdec->p_context->aspect_ratio_info,
                           ffmpeg_PixFmtToChroma(p_vdec->p_context->pix_fmt)) )
    {
        p_vdec->p_vout = ffmpeg_CreateVout( p_vdec,
                           p_vdec->p_context->width,
                           p_vdec->p_context->height,
                           p_vdec->p_context->aspect_ratio_info,
                           ffmpeg_PixFmtToChroma(p_vdec->p_context->pix_fmt) );
        if( !p_vdec->p_vout )
        {
            msg_Err( p_vdec->p_fifo, "cannot create vout" );
            p_vdec->p_fifo->b_error = 1; /* abort */
            return -1;
        }
    }

    /* Get a new picture */
    while( !(p_pic = vout_CreatePicture( p_vdec->p_vout, 0, 0, 0 ) ) )
    {
        if( p_vdec->p_fifo->b_die || p_vdec->p_fifo->b_error )
        {
            return -1;
        }
        msleep( VOUT_OUTMEM_SLEEP );
    }

    /* FIXME: We keep the last picture linked until the current one is decoded,
     * this trick won't work with streams with B frames though. */
    vout_LinkPicture( p_vdec->p_vout, p_pic );
    if( p_vdec->p_secondlastpic )
        vout_UnlinkPicture( p_vdec->p_vout, p_vdec->p_secondlastpic );
    p_vdec->p_secondlastpic = p_vdec->p_lastpic;
    p_vdec->p_lastpic = p_pic;

    avctx->draw_horiz_band= NULL;
    avctx->dr_buffer[0]= p_pic->p[0].p_pixels;
    avctx->dr_buffer[1]= p_pic->p[1].p_pixels;
    avctx->dr_buffer[2]= p_pic->p[2].p_pixels;

    avctx->dr_stride   = p_pic->p[0].i_pitch;
    avctx->dr_uvstride = p_pic->p[1].i_pitch;

    avctx->dr_opaque_frame = p_pic;

    /* FIXME: this variable is used to determine if a macro-block to be written
     * can be skipped. The idea behind this is that if a macro-block hasn't
     * changed and all the frame-buffers already have the value of this
     * macro-block, then we can skip the writting.
     * But currently we cannot ensure this is the case, so we decide to write
     * everything. */
    avctx->dr_ip_buffer_count = 999;

    return 0;
#endif
}
