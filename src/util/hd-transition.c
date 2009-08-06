/*
 * This file is part of hildon-desktop
 *
 * Copyright (C) 2008 Nokia Corporation.
 *
 * Author:  Gordon Williams <gordon.williams@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <sys/inotify.h>

#include <clutter/clutter.h>
#include <canberra.h>

#include "hd-transition.h"
#include "hd-comp-mgr.h"
#include "hd-gtk-style.h"
#include "hd-render-manager.h"
#include "hildon-desktop.h"
#include "hd-theme.h"
#include "hd-title-bar.h"
#include "hd-clutter-cache.h"
#include "tidy/tidy-sub-texture.h"

#include "hd-app.h"
#include "hd-volume-profile.h"
#include "hd-util.h"

/* The master of puppets */
#define TRANSITIONS_INI "/usr/share/hildon-desktop/transitions.ini"

typedef struct _HDEffectData
{
  MBWMCompMgrClientEvent   event;
  ClutterTimeline          *timeline;
  MBWMCompMgrClutterClient *cclient;
  ClutterActor             *cclient_actor;
  /* In subview transitions, this is the ORIGINAL (non-subview) view */
  MBWMCompMgrClutterClient *cclient2;
  ClutterActor             *cclient2_actor;
  HdCompMgr                *hmgr;
  /* original/expected position of application/menu */
  ClutterGeometry           geo;
  /* used in rotate_screen to set the direction (and amount) of movement */
  float                     angle;
  /* Any extra particles if they are used for this effect */
  ClutterActor             *particles[HDCM_UNMAP_PARTICLES];
} HDEffectData;

/* Describes the state of hd_transition_rotating_fsm(). */
static struct
{
  MBWindowManager *wm;

  /*
   * @direction:      Where we're going now.
   * @new_direction:  Reaching the next @phase where to go.
   *                  Used to override half-finished transitions.
   *                  *_fsm() needs to check it at the end of each @phase.
   */
  enum
  {
    GOTO_LANDSCAPE,
    GOTO_PORTRAIT,
  } direction, new_direction;

  /*
   * What is *_fsm() currently doing:
   * -- #IDLE:      nothing, we're sitting in landscape or portrait
   * -- #FADE_OUT:  hd_transition_fade_and_rotate() is fading out
   * -- #WAITING:   for X to finish reconfiguring the screen
   * -- #FADE_IN:   second hd_transition_fade_and_rotate() is in progress
   */
  enum
  {
    IDLE,
    FADE_OUT,
    WAITING,
    FADE_IN,
  } phase;

  /*
   * @goto_state when we've %FADE_OUT:d.  Set by
   * hd_transition_rotate_screen_and_change_state()
   * Its initial value is %HDRM_STATE_UNDEFINED, which means don't
   * change the state. */
  HDRMStateEnum goto_state;

  /* In the WAITING state we have a timer that calls us back a few ms
   * after the last damage event. This is the id, as we need to restart
   * it whenever we get another damage event. */
  guint timeout_id;

  /* This timer counts from when we first entered the WAITING state,
   * so if we are continually getting damage we don't just hang there. */
  GTimer *timer;

  /* If a client (like CallUI) was forcing portrait mode and it quits,
   * we leave it visible and put an HDEffectData this list. During blanking we
   * remove the actor by calling hd_transition_completed. */
  GList *effects_waiting;
} Orientation_change;

/* If %TRUE keep reloading transitions.ini until we can
 * and we can watch it. */
static gboolean transitions_ini_is_dirty;

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */

/* amt goes from 0->1, and the result goes mostly from 0->1 with a bit of
 * overshoot at the end */
float
hd_transition_overshoot(float x)
{
  float smooth_ramp, converge;
  float amt;
  int offset;
  offset = (int)x;
  amt = x-offset;
  smooth_ramp = 1.0f - cos(amt*3.141592); // 0 <= smooth_ramp <= 2
  converge = sin(0.5*3.141592*(1-amt)); // 0 <= converve <= 1
  return offset + (smooth_ramp*0.675)*converge + (1-converge);
}

/* amt goes from 0->1, and the result goes from 0->1 smoothly */
float
hd_transition_smooth_ramp(float amt)
{
  if (amt>0 && amt<1)
    return (1.0f - cos(amt*3.141592)) * 0.5f;
  return amt;
}

float
hd_transition_ease_in(float amt)
{
  if (amt>0 && amt<1)
    return (1.0f - cos(amt*3.141592*0.5));
  return amt;
}

float
hd_transition_ease_out(float amt)
{
  if (amt>0 && amt<1)
    return cos((1-amt)*3.141592*0.5);
  return amt;
}

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */

static ClutterTimeline *
hd_transition_timeline_new(const gchar *transition,
                           MBWMCompMgrClientEvent event,
                           gint default_length)
{
  const char *key =
    event==MBWMCompMgrClientEventMap ?"duration_in":"duration_out";
  return clutter_timeline_new_for_duration (
      hd_transition_get_int(transition, key, default_length) );
}

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */

/* For the animated progress indicator in the title bar */
void
on_decor_progress_timeline_new_frame(ClutterTimeline *timeline,
                                     gint frame_num,
                                     ClutterActor *progress_texture)
{
  if (TIDY_IS_SUB_TEXTURE(progress_texture) &&
      CLUTTER_ACTOR_IS_VISIBLE(progress_texture))
    {
      /* The progress animation is a series of frames packed
       * into a texture - like a film strip
       */
      ClutterGeometry progress_region =
         {HD_THEME_IMG_PROGRESS_SIZE*frame_num, 0,
          HD_THEME_IMG_PROGRESS_SIZE, HD_THEME_IMG_PROGRESS_SIZE };

      tidy_sub_texture_set_region(
          TIDY_SUB_TEXTURE(progress_texture),
          &progress_region);

      /* FIXME: We really want to set this to queue damage with an area -
       * like we do for windows. Otherwise we end up updating the whole
       * screen for this. */
      clutter_actor_queue_redraw(progress_texture);
    }
}

static void
on_popup_timeline_new_frame(ClutterTimeline *timeline,
                            gint frame_num, HDEffectData *data)
{
  float amt;
  ClutterActor *actor, *filler;
  int status_low, status_high;
  float status_pos;
  gboolean pop_top, pop_bottom; /* pop in from the top, or the bottom */

  float overshoot;

  actor = data->cclient_actor;
  if (!CLUTTER_IS_ACTOR(actor))
    return;
  filler = data->particles[0];

  /* We need to get geometry each frame as often windows have
   * a habit of changing size while they move. If we have filler
   * we remove it first, so it doesn't affect the geometry. */
  if (filler && clutter_actor_get_parent(filler))
    clutter_container_remove_actor(
        CLUTTER_CONTAINER(clutter_actor_get_parent(filler)), filler);
  ClutterGeometry geo;
  clutter_actor_get_geometry(actor, &geo);

  pop_top = geo.y==0;
  pop_bottom = geo.y+geo.height==hd_comp_mgr_get_current_screen_height();
  if (pop_top && pop_bottom)
    pop_top = FALSE;
  amt =  (float)clutter_timeline_get_progress(timeline);
  /* reverse if we're removing this */
  if (data->event == MBWMCompMgrClientEventUnmap)
    amt = 1-amt;

  overshoot = hd_transition_overshoot(amt);

  if (pop_top)
    {
      status_low = -geo.height;
      status_high = geo.y;
    }
  else if (pop_bottom)
    {
      status_low = geo.y+geo.height;
      status_high = geo.y;
    }
  else
    {
      status_low = geo.y;
      status_high = geo.y;
    }
  status_pos = status_low*(1-overshoot) + status_high*overshoot;

  clutter_actor_set_anchor_pointu(actor, 0,
      CLUTTER_INT_TO_FIXED(geo.y) - CLUTTER_FLOAT_TO_FIXED(status_pos));
  clutter_actor_set_opacity(actor, (int)(255*amt));

  /* use a slither of filler to fill in the gap where the menu
   * has jumped a bit too far up */
  if (filler &&
      ((status_pos>status_high && pop_top) ||
       (status_pos<status_high && pop_bottom)))
    {
      // re-add the filler (see above)
      if (CLUTTER_IS_CONTAINER(actor))
        clutter_container_add_actor(CLUTTER_CONTAINER(actor), filler);
      clutter_actor_show(filler);
      if (pop_top)
        {
          clutter_actor_set_positionu(filler,
                    CLUTTER_INT_TO_FIXED(0),
                    CLUTTER_FLOAT_TO_FIXED(status_high-status_pos));
          clutter_actor_set_sizeu(filler,
                    CLUTTER_INT_TO_FIXED(geo.width),
                    CLUTTER_FLOAT_TO_FIXED(status_pos-status_high));
        }
      else if (pop_bottom)
        {
          clutter_actor_set_positionu(filler,
                    CLUTTER_INT_TO_FIXED(0),
                    CLUTTER_INT_TO_FIXED(geo.height));
          clutter_actor_set_sizeu(filler,
                    CLUTTER_INT_TO_FIXED(geo.width),
                    CLUTTER_FLOAT_TO_FIXED(status_high-status_pos));
        }
    }
}

static void
on_fade_timeline_new_frame(ClutterTimeline *timeline,
                            gint frame_num, HDEffectData *data)
{
  float amt;
  ClutterActor *actor;

  actor = data->cclient_actor;
  if (!CLUTTER_IS_ACTOR(actor))
    return;

  amt =  (float)clutter_timeline_get_progress(timeline);
  /* reverse if we're removing this */
  if (data->event == MBWMCompMgrClientEventUnmap)
    amt = 1-amt;
  amt = hd_transition_smooth_ramp(amt);

  clutter_actor_set_opacity(actor, (int)(255*amt));
}

static void
on_close_timeline_new_frame(ClutterTimeline *timeline,
                            gint frame_num, HDEffectData *data)
{
  float amt;
  ClutterActor *actor;
  float amtx, amty, amtp;
  int centrex, centrey;
  float particle_opacity, particle_radius, particle_scale;
  gint i;

  actor = data->cclient_actor;
  if (!CLUTTER_IS_ACTOR(actor))
    return;

  amt = (float)clutter_timeline_get_progress(timeline);

  amtx = 1.6 - amt*2.5; // shrink in x
  amty = 1 - amt*2.5; // shrink in y
  amtp = amt*2 - 1; // particles
  if (amtx<0) amtx=0;
  if (amtx>1) amtx=1;
  if (amty<0) amty=0;
  if (amty>1) amty=1;
  if (amtp<0) amtp=0;
  if (amtp>1) amtp=1;
  /* smooth out movement */
  amtx = (1-cos(amtx * 3.141592)) * 0.45f + 0.1f;
  amty = (1-cos(amty * 3.141592)) * 0.45f + 0.1f;
  particle_opacity = sin(amtp * 3.141592);
  particle_radius = 8 + (1-cos(amtp * 3.141592)) * 32.0f;

  centrex =  data->geo.x + data->geo.width / 2 ;
  centrey =  data->geo.y + data->geo.height / 2 ;
  /* set app location and fold up like a turned-off TV.
   * @actor is anchored in the middle so it needn't be repositioned */
  clutter_actor_set_scale(actor, amtx, amty);
  clutter_actor_set_opacity(actor, (int)(255 * (1-amtp)));
  /* do sparkles... */
  particle_scale = 1-amtp*0.5;
  for (i=0;i<HDCM_UNMAP_PARTICLES;i++)
    if (data->particles[i] && (amtp > 0) && (amtp < 1))
      {
        /* space particles semi-randomly and rotate once */
        float ang = i * 15 +
                    amtp * 3.141592f / 2;
        float radius = particle_radius * (i+1) / HDCM_UNMAP_PARTICLES;
        /* twinkle effect */
        float opacity = particle_opacity * ((1-cos(amt*50+i)) * 0.5f);
        clutter_actor_show( data->particles[i] );
        clutter_actor_set_opacity(data->particles[i],
                (int)(255 * opacity));
        clutter_actor_set_scale(data->particles[i],
                particle_scale, particle_scale);

        clutter_actor_set_positionu(data->particles[i],
                CLUTTER_FLOAT_TO_FIXED(centrex + sin(ang) * radius),
                CLUTTER_FLOAT_TO_FIXED(centrey + cos(ang) * radius));
      }
    else
      if (data->particles[i])
	clutter_actor_hide( data->particles[i] );
}

static void
on_notification_timeline_new_frame(ClutterTimeline *timeline,
                                   gint frame_num, HDEffectData *data)
{
  float now;
  ClutterActor *actor;
  guint width, height;

  actor = data->cclient_actor;
  if (!CLUTTER_IS_ACTOR(actor))
    return;

  clutter_actor_get_size(actor, &width, &height);

  now = frame_num / (float)clutter_timeline_get_n_frames(timeline);

  if (data->event == MBWMCompMgrClientEventUnmap)
    {
      float t, thr;

      /*
       * Timeline is broken into two pieces.  The first part takes
       * @thr seconds and during that the notification actor is moved
       * to its final place,  The second part is much shorter and
       * during that it's faded to nothingness.
       */
      thr = 400.0 / (150+400);

      if (now < thr)
        { /* fade, move, resize */
          gint cx, cy;
          float sx, sy;

          /*
           * visual geometry: 366x88+112+0 -> 96x23+8+17
           *                  scale it down proportionally
           *                  and place it in the middle of the tasks button
           *                  leaving 8 pixels left and right
           * opacity:         1 -> 0.75
           * use smooth ramping
           */
          t = hd_transition_smooth_ramp(now / thr);
          cx = ( 8 - clutter_actor_get_x(actor))*t;
          cy = (17 - clutter_actor_get_y(actor))*t;
          sx = ((96.0/width  - 1)*t + 1);
          sy = ((23.0/height - 1)*t + 1);

          clutter_actor_set_scale (actor, sx, sy);
          clutter_actor_set_anchor_point (actor, -cx/sx, -cy/sy);
          clutter_actor_set_opacity(actor, 255 * ((0.75 - 1)*t + 1));
        }
      else
        { /* fade: 0.75 -> 0 linearly */
          t = (now - thr) / (1.0 - thr);
          clutter_actor_set_opacity(actor, 255 * (-0.75*t + 0.75));
        }
    }
  else
    {
      /* Opening Animation - we fade in, and move in from the top-right
       * edge of the screen in an arc */
      float amt = hd_transition_smooth_ramp(now);
      float scale =  1 + (1-amt)*0.5f;
      float ang = amt * 3.141592f * 0.5f;
      float corner_x = (hd_comp_mgr_get_current_screen_width()*0.5f
                        - HD_COMP_MGR_TOP_LEFT_BTN_WIDTH) * cos(ang);
      float corner_y = (sin(ang)-1) * height;
      /* We set anchor point so if the notification resizes/positions
       * in flight, we're ok.  NOTE that the position of the actor
       * (get_position()) still matters, and it is LEFT_BIN_WIDTH. */
      clutter_actor_set_opacity(actor, (int)(255*amt));
      clutter_actor_set_scale(actor, scale, scale);
      clutter_actor_set_anchor_pointu(actor,
               CLUTTER_FLOAT_TO_FIXED( -corner_x / scale ),
               CLUTTER_FLOAT_TO_FIXED( -corner_y / scale ));
    }
}

static void
on_subview_timeline_new_frame(ClutterTimeline *timeline,
                              gint frame_num, HDEffectData *data)
{
  float amt;
  gint n_frames;
  ClutterActor *subview_actor = 0, *main_actor = 0;

  if (data->cclient)
    subview_actor = data->cclient_actor;
  if (data->cclient2)
    main_actor = data->cclient2_actor;

  n_frames = clutter_timeline_get_n_frames(timeline);
  amt = frame_num / (float)n_frames;
  amt = hd_transition_smooth_ramp( amt );
  if (data->event == MBWMCompMgrClientEventUnmap)
    amt = 1-amt;

  {
    float corner_x;
    corner_x = (1-amt) * hd_comp_mgr_get_current_screen_width();
    if (subview_actor)
      {
        clutter_actor_set_anchor_pointu(subview_actor,
           CLUTTER_FLOAT_TO_FIXED( -corner_x ),
           CLUTTER_FLOAT_TO_FIXED( 0 ) );
        /* we have to show this actor, because it'll get hidden by the
         * render manager visibility test if not. */
        clutter_actor_show(subview_actor);
      }
    if (main_actor)
      {
        clutter_actor_set_anchor_pointu(main_actor,
           CLUTTER_FLOAT_TO_FIXED( -(corner_x - hd_comp_mgr_get_current_screen_width()) ),
           CLUTTER_FLOAT_TO_FIXED( 0 ) );
        /* we have to show this actor, because it'll get hidden by the
         * render manager visibility test if not. */
        clutter_actor_show(main_actor);
      }
  }

  /* if we're at the last frame, return our actors to the correct places) */
  if (frame_num == n_frames)
    {
      if (subview_actor)
        {
          clutter_actor_set_anchor_pointu(subview_actor, 0, 0);
          if (data->event == MBWMCompMgrClientEventUnmap)
            clutter_actor_hide(subview_actor);
        }
      if (main_actor)
        {
          clutter_actor_set_anchor_pointu(main_actor, 0, 0);
          /* hide the correct actor - as we overrode the visibility test in hdrm */
          if (data->event == MBWMCompMgrClientEventMap)
            clutter_actor_hide(main_actor);
        }
    }
}

static void
on_rotate_screen_timeline_new_frame(ClutterTimeline *timeline,
                                    gint frame_num, HDEffectData *data)
{
  float amt, dim_amt, angle;
  gint n_frames;
  ClutterActor *actor;

  n_frames = clutter_timeline_get_n_frames(timeline);
  amt = frame_num / (float)n_frames;
  // we want to ease in, but speed up as we go - X^3 does this nicely
  amt = amt*amt*amt;
  if (data->event == MBWMCompMgrClientEventUnmap)
    amt = 1-amt;
  /* dim=1 -> screen is black, dim=0 -> normal. Only
   * dim out right at the end of the animation */
  dim_amt = amt*4 - 3;
  if (dim_amt<0)
    dim_amt = 0;
  angle = data->angle * amt;

  actor = CLUTTER_ACTOR(hd_render_manager_get());
  clutter_actor_set_rotation(actor,
      hd_comp_mgr_is_portrait () ? CLUTTER_Y_AXIS : CLUTTER_X_AXIS,
      frame_num < n_frames ? angle : 0,
      hd_comp_mgr_get_current_screen_width()/2,
      hd_comp_mgr_get_current_screen_height()/2, 0);
  clutter_actor_set_depthu(actor, -CLUTTER_FLOAT_TO_FIXED(amt*150));
  /* use this actor to dim out the screen */
  clutter_actor_raise_top(data->particles[0]);
  clutter_actor_set_opacity(data->particles[0], (int)(dim_amt*255));
}

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */

/* #ClutterStage's notify::allocation callback to notice if we are
 * switching between landscape and portrait modes duing an effect. */
static void
on_screen_size_changed (ClutterActor *stage, GParamSpec *unused,
                        HDEffectData *data)
{
  gint tmp;
  guint scrw, scrh;
  ClutterActor *actor;

  /* Rotate @actor back to the mode it is layed out for.
   * Assume it's anchored in the middle. */
  clutter_actor_get_size (stage, &scrw, &scrh);
  actor = mb_wm_comp_mgr_clutter_client_get_actor (data->cclient);

  /* It is very interesting to observe the dualism here. */
  if (scrw > scrh)
    { /* Coming from portrait to landscape. */
      clutter_actor_set_rotation (actor, CLUTTER_Z_AXIS, -90, 0, 0, 0);

      tmp = data->geo.x;
      data->geo.x = data->geo.y;
      data->geo.y = scrh - (tmp + data->geo.width);
    }
  else
    { /* Coming from landscape to portrait. */
      clutter_actor_set_rotation (actor, CLUTTER_Z_AXIS, +90, 0, 0, 0);

      tmp = data->geo.y;
      data->geo.y = data->geo.x;
      data->geo.x = scrw - (tmp + data->geo.height);
    }

  tmp = data->geo.width;
  data->geo.width = data->geo.height;
  data->geo.height = tmp;

  clutter_actor_set_position (actor,
                              data->geo.x + data->geo.width/2,
                              data->geo.y + data->geo.height/2);
}

static void
hd_transition_completed (ClutterTimeline* timeline, HDEffectData *data)
{
  gint i;
  HdCompMgr *hmgr = HD_COMP_MGR (data->hmgr);

  if (data->cclient)
    {
      HD_COMP_MGR_CLIENT (data->cclient)->effect = NULL;
      mb_wm_comp_mgr_clutter_client_unset_flags (data->cclient,
                                        MBWMCompMgrClutterClientDontUpdate |
                                        MBWMCompMgrClutterClientEffectRunning);
      mb_wm_object_unref (MB_WM_OBJECT (data->cclient));
      if (data->event == MBWMCompMgrClientEventUnmap && data->cclient_actor)
        {
          ClutterActor *parent = clutter_actor_get_parent(data->cclient_actor);
          if (CLUTTER_IS_CONTAINER(parent))
            clutter_container_remove_actor(
                CLUTTER_CONTAINER(parent), data->cclient_actor );
        }
    }

  if (data->cclient_actor)
    g_object_unref(data->cclient_actor);

  if (data->cclient2)
    {
      HD_COMP_MGR_CLIENT (data->cclient2)->effect = NULL;
      mb_wm_comp_mgr_clutter_client_unset_flags (data->cclient2,
                                        MBWMCompMgrClutterClientDontUpdate |
                                        MBWMCompMgrClutterClientEffectRunning);
      mb_wm_object_unref (MB_WM_OBJECT (data->cclient2));
    }

  if (data->cclient2_actor)
    g_object_unref(data->cclient2_actor);

/*   dump_clutter_tree (CLUTTER_CONTAINER (clutter_stage_get_default()), 0); */

  if (timeline)
    g_object_unref ( timeline );

  if (hmgr)
    hd_comp_mgr_set_effect_running(hmgr, FALSE);

  for (i=0;i<HDCM_UNMAP_PARTICLES;i++)
    if (data->particles[i]) {
      // if actor was in a group, remove it
      if (CLUTTER_IS_CONTAINER(clutter_actor_get_parent(data->particles[i])))
             clutter_container_remove_actor(
               CLUTTER_CONTAINER(clutter_actor_get_parent(data->particles[i])),
               data->particles[i]);
      g_object_unref(data->particles[i]); // unref ourselves
      data->particles[i] = 0; // for safety, set pointer to 0
    }

  g_signal_handlers_disconnect_by_func (clutter_stage_get_default (),
                                        G_CALLBACK (on_screen_size_changed),
                                        data);

  g_free (data);
}

void
hd_transition_popup(HdCompMgr                  *mgr,
                    MBWindowManagerClient      *c,
                    MBWMCompMgrClientEvent     event)
{
  MBWMCompMgrClutterClient * cclient;
  ClutterActor             * actor;
  HDEffectData             * data;
  ClutterGeometry            geo;
  ClutterColor col;

  cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);
  actor = mb_wm_comp_mgr_clutter_client_get_actor (cclient);
  if (!actor)
    return;
  clutter_actor_get_geometry(actor, &geo);

  /* Need to store also pointer to the manager, as by the time
   * the effect finishes, the back pointer in the cm_client to
   * MBWindowManagerClient is not longer valid/set.
   */
  data = g_new0 (HDEffectData, 1);
  data->event = event;
  data->cclient = mb_wm_object_ref (MB_WM_OBJECT (cclient));
  data->cclient_actor = g_object_ref (actor);
  data->hmgr = HD_COMP_MGR (mgr);
  data->timeline =
        g_object_ref( hd_transition_timeline_new("popup", event, 250) );
  g_signal_connect (data->timeline, "new-frame",
                        G_CALLBACK (on_popup_timeline_new_frame), data);
  g_signal_connect (data->timeline, "completed",
                        G_CALLBACK (hd_transition_completed), data);
  data->geo = geo;

  mb_wm_comp_mgr_clutter_client_set_flags (cclient,
                              MBWMCompMgrClutterClientDontUpdate |
                              MBWMCompMgrClutterClientEffectRunning);

  hd_comp_mgr_set_effect_running(mgr, TRUE);

  /* Add actor for the background when we pop a bit too far */
  data->particles[0] = g_object_ref(clutter_rectangle_new());
  hd_gtk_style_get_bg_color(HD_GTK_BUTTON_SINGLETON, GTK_STATE_NORMAL,
                              &col);
  clutter_rectangle_set_color(CLUTTER_RECTANGLE(data->particles[0]),
                              &col);

  /* first call to stop flicker */
  on_popup_timeline_new_frame(data->timeline, 0, data);
  clutter_timeline_start (data->timeline);
}

void
hd_transition_fade(HdCompMgr                  *mgr,
                   MBWindowManagerClient      *c,
                   MBWMCompMgrClientEvent     event)
{
  MBWMCompMgrClutterClient * cclient;
  HDEffectData             * data;

  cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);

  /* Need to store also pointer to the manager, as by the time
   * the effect finishes, the back pointer in the cm_client to
   * MBWindowManagerClient is not longer valid/set.
   */
  data = g_new0 (HDEffectData, 1);
  data->event = event;
  data->cclient = mb_wm_object_ref (MB_WM_OBJECT (cclient));
  data->cclient_actor = g_object_ref (
      mb_wm_comp_mgr_clutter_client_get_actor( data->cclient ) );
  data->hmgr = HD_COMP_MGR (mgr);
  data->timeline =
        g_object_ref( hd_transition_timeline_new("fade", event, 250) );
  g_signal_connect (data->timeline, "new-frame",
                        G_CALLBACK (on_fade_timeline_new_frame), data);
  g_signal_connect (data->timeline, "completed",
                        G_CALLBACK (hd_transition_completed), data);

  mb_wm_comp_mgr_clutter_client_set_flags (cclient,
                              MBWMCompMgrClutterClientDontUpdate |
                              MBWMCompMgrClutterClientEffectRunning);
  hd_comp_mgr_set_effect_running(mgr, TRUE);

  /* first call to stop flicker */
  on_fade_timeline_new_frame(data->timeline, 0, data);
  clutter_timeline_start (data->timeline);
}

void
hd_transition_close_app (HdCompMgr                  *mgr,
                         MBWindowManagerClient      *c)
{
  MBWMClientType c_type = MB_WM_CLIENT_CLIENT_TYPE (c);
  MBWMCompMgrClutterClient * cclient;
  ClutterActor             * actor;
  HdApp                    * app;
  HDEffectData             * data;
  ClutterGeometry            geo;
  ClutterContainer         * parent;
  gint                       i;

  /* proper app close animation */
  if (c_type != MBWMClientTypeApp)
    return;

  /* The switcher will do the effect if it's active,
   * don't interfere. */
  if (hd_render_manager_get_state()==HDRM_STATE_TASK_NAV)
    return;

  /* Don't do the unmap transition if it's a secondary. */
  app = HD_APP (c);
  if (app->stack_index > 0 && app->leader != app)
    {
      /* FIXME: Transitions. */
      g_debug ("%s: Skip non-leading secondary window.", __FUNCTION__);
      return;
    }

  cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);
  actor = mb_wm_comp_mgr_clutter_client_get_actor (cclient);
  if (!actor || !CLUTTER_ACTOR_IS_VISIBLE(actor))
    return;

  /* Don't bother for anything tiny */
  clutter_actor_get_geometry(actor, &geo);
  if (geo.width<16 || geo.height<16)
    return;

  /*
   * Need to store also pointer to the manager, as by the time
   * the effect finishes, the back pointer in the cm_client to
   * MBWindowManagerClient is not longer valid/set.
   *
   * It is possible that during the effect we leave portrait mode,
   * so be prepared for it.
   */
  data = g_new0 (HDEffectData, 1);
  data->event = MBWMCompMgrClientEventUnmap;
  data->cclient = mb_wm_object_ref (MB_WM_OBJECT (cclient));
  data->cclient_actor = g_object_ref (actor);
  data->hmgr = HD_COMP_MGR (mgr);
  data->timeline = g_object_ref(
                clutter_timeline_new_for_duration (
                    hd_transition_get_int("app_close", "duration", 500) ) );
  g_signal_connect (data->timeline, "new-frame",
                    G_CALLBACK (on_close_timeline_new_frame), data);
  g_signal_connect (clutter_stage_get_default (), "notify::allocation",
                    G_CALLBACK (on_screen_size_changed), data);
  g_signal_connect (data->timeline, "completed",
                    G_CALLBACK (hd_transition_completed), data);
  data->geo = geo;

  mb_wm_comp_mgr_clutter_client_set_flags (cclient,
                                  MBWMCompMgrClutterClientDontUpdate |
                                  MBWMCompMgrClutterClientEffectRunning);

  parent = hd_render_manager_get_front_group();
  /* reparent our actor so it will be visible when we switch views */
  clutter_actor_reparent(actor, CLUTTER_ACTOR(parent));
  clutter_actor_lower_bottom(actor);
  clutter_actor_move_anchor_point_from_gravity(actor, CLUTTER_GRAVITY_CENTER);

  for (i = 0; i < HDCM_UNMAP_PARTICLES; ++i)
    {
      data->particles[i] = hd_clutter_cache_get_texture(
          HD_THEME_IMG_CLOSING_PARTICLE, TRUE);
      if (data->particles[i])
        {
          g_object_ref(data->particles[i]);
          clutter_actor_set_anchor_point_from_gravity(data->particles[i],
                                                      CLUTTER_GRAVITY_CENTER);
          clutter_container_add_actor(parent, data->particles[i]);
          clutter_actor_hide(data->particles[i]);
        }
    }

  hd_comp_mgr_set_effect_running(mgr, TRUE);
  clutter_timeline_start (data->timeline);

  hd_transition_play_sound (HDCM_WINDOW_CLOSED_SOUND);
}

void
hd_transition_close_app_before_rotate (HdCompMgr                  *hmgr,
                                       MBWindowManagerClient      *c)
{
  MBWMClientType c_type = MB_WM_CLIENT_CLIENT_TYPE (c);
  MBWMCompMgrClutterClient * cclient;
  HDEffectData             * data;
  ClutterActor             * actor;
  ClutterContainer         * parent;

  /* proper app close animation */
  if (c_type != MBWMClientTypeApp)
    return;

  /* The switcher will do the effect if it's active,
   * don't interfere. */
  if (hd_render_manager_get_state()==HDRM_STATE_TASK_NAV)
    return;

  cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);
  actor = mb_wm_comp_mgr_clutter_client_get_actor (cclient);
  if (!actor || !CLUTTER_ACTOR_IS_VISIBLE(actor))
    return;

  data = g_new0 (HDEffectData, 1);
  data->event = MBWMCompMgrClientEventUnmap;
  data->cclient = mb_wm_object_ref (MB_WM_OBJECT (cclient));
  data->cclient_actor = g_object_ref (actor);
  data->hmgr = hmgr;


  mb_wm_comp_mgr_clutter_client_set_flags (cclient,
                                    MBWMCompMgrClutterClientDontUpdate |
                                    MBWMCompMgrClutterClientDontShow |
                                    MBWMCompMgrClutterClientEffectRunning);

  /* reparent our actor so it will be visible when we switch views */
  parent = hd_render_manager_get_front_group();
  clutter_actor_reparent(actor,
      CLUTTER_ACTOR(parent));
  clutter_actor_lower_bottom(actor);
  /* Also add a fake titlebar background, as the real one will disappear
   * immediately because the app has closed. */
  data->particles[0] = hd_title_bar_create_fake(HD_COMP_MGR_LANDSCAPE_HEIGHT);
  clutter_container_add_actor(parent, data->particles[0]);

  Orientation_change.effects_waiting = g_list_append(
      Orientation_change.effects_waiting, data);
  hd_comp_mgr_set_effect_running(hmgr, TRUE);

  hd_transition_play_sound (HDCM_WINDOW_CLOSED_SOUND);
}

void
hd_transition_notification(HdCompMgr                  *mgr,
                           MBWindowManagerClient      *c,
                           MBWMCompMgrClientEvent     event)
{
  MBWMCompMgrClutterClient * cclient;
  HDEffectData             * data;

  cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);

  /* Need to store also pointer to the manager, as by the time
   * the effect finishes, the back pointer in the cm_client to
   * MBWindowManagerClient is not longer valid/set.
   */
  data = g_new0 (HDEffectData, 1);
  data->event = event;
  data->cclient = mb_wm_object_ref (MB_WM_OBJECT (cclient));
  data->cclient_actor = g_object_ref (
      mb_wm_comp_mgr_clutter_client_get_actor( data->cclient ) );
  data->hmgr = HD_COMP_MGR (mgr);
  data->timeline =
        g_object_ref( hd_transition_timeline_new("notification", event, 500) );

  g_signal_connect (data->timeline, "new-frame",
                        G_CALLBACK (on_notification_timeline_new_frame), data);
  g_signal_connect (data->timeline, "completed",
                        G_CALLBACK (hd_transition_completed), data);

  mb_wm_comp_mgr_clutter_client_set_flags (cclient,
                              MBWMCompMgrClutterClientDontUpdate |
                              MBWMCompMgrClutterClientEffectRunning);
  hd_comp_mgr_set_effect_running(mgr, TRUE);

  /* first call to stop flicker */
  on_notification_timeline_new_frame(data->timeline, 0, data);
  clutter_timeline_start (data->timeline);
}

void
hd_transition_subview(HdCompMgr                  *mgr,
                      MBWindowManagerClient      *subview,
                      MBWindowManagerClient      *mainview,
                      MBWMCompMgrClientEvent     event)
{
  MBWMCompMgrClutterClient * cclient_subview;
  MBWMCompMgrClutterClient * cclient_mainview;
  gboolean                   mainview_in_trans, subview_in_trans;
  HDEffectData             * data;

  if (subview == mainview)
    { /* This happens sometimes for unknown reason. */
      g_critical ("hd_transition_subview: mainview == subview == %p",
                  subview);
      return;
    }
  if (!subview || !subview->cm_client || !mainview || !mainview->cm_client
      || !STATE_IS_APP(hd_render_manager_get_state()))
    return;

  cclient_subview = MB_WM_COMP_MGR_CLUTTER_CLIENT (subview->cm_client);
  cclient_mainview = MB_WM_COMP_MGR_CLUTTER_CLIENT (mainview->cm_client);

  /*
   * Handle views which are already in transition.
   * Two special cases are handled:
   * the client pushes a series of windows or it pops a series of windows.
   * The transitions would overlap but we can replace the finally-to-be-shown
   * actor, making it smooth.
   *
   * NOTE We exploit that currently only this transition sets
   * %HdCompMgrClient::effect and we use it to recognize ongoing
   * subview transitions.
   */
  subview_in_trans = mb_wm_comp_mgr_clutter_client_get_flags (cclient_subview)
    & MBWMCompMgrClutterClientEffectRunning;
  mainview_in_trans = mb_wm_comp_mgr_clutter_client_get_flags (cclient_mainview)
    & MBWMCompMgrClutterClientEffectRunning;
  if (subview_in_trans && mainview_in_trans)
    return;
  if (mainview_in_trans)
    { /* Is the mainview we want to leave sliding in? */
      if (event == MBWMCompMgrClientEventMap
          && (data = HD_COMP_MGR_CLIENT (cclient_mainview)->effect)
          && data->event == MBWMCompMgrClientEventMap
          && data->cclient == cclient_mainview)
        {
          /* Replace the effect's subview with ours. */
          /* Release @cclient and @cclient_actor. */
          clutter_actor_hide (data->cclient_actor);
          clutter_actor_set_anchor_pointu (data->cclient_actor, 0, 0);
          g_object_unref (data->cclient_actor);
          HD_COMP_MGR_CLIENT (data->cclient)->effect = NULL;
          mb_wm_comp_mgr_clutter_client_unset_flags (data->cclient,
                                      MBWMCompMgrClutterClientDontUpdate |
                                      MBWMCompMgrClutterClientEffectRunning);
          mb_wm_object_unref (MB_WM_OBJECT (data->cclient));

          /* Set @cclient_subview. */
          data->cclient = mb_wm_object_ref (MB_WM_OBJECT (cclient_subview));
          data->cclient_actor = g_object_ref (
              mb_wm_comp_mgr_clutter_client_get_actor (cclient_subview));
          mb_wm_comp_mgr_clutter_client_set_flags (cclient_subview,
                                      MBWMCompMgrClutterClientDontUpdate |
                                      MBWMCompMgrClutterClientEffectRunning);
          HD_COMP_MGR_CLIENT (cclient_subview)->effect = data;
        }
      return;
    }
  if (subview_in_trans) /* This is almost the same code. */
    { /* Is the subview we want to leave sliding in? */
      if (event == MBWMCompMgrClientEventUnmap
          && (data = HD_COMP_MGR_CLIENT (cclient_subview)->effect)
          && data->event == MBWMCompMgrClientEventUnmap
          && data->cclient2 == cclient_subview)
        {
          ClutterActor *o;

          /* Replace the effect's mainview with ours. */
          /* Release @cclient2 and @cclient2_actor. */
          clutter_actor_hide (data->cclient2_actor);
          clutter_actor_set_anchor_pointu (data->cclient2_actor, 0, 0);
          g_object_unref (o = data->cclient2_actor);
          HD_COMP_MGR_CLIENT (data->cclient2)->effect = NULL;
          mb_wm_comp_mgr_clutter_client_unset_flags (data->cclient2,
                                      MBWMCompMgrClutterClientDontUpdate |
                                      MBWMCompMgrClutterClientEffectRunning);
          mb_wm_object_unref (MB_WM_OBJECT (data->cclient2));

          /* Set @cclient_mainview. */
          data->cclient2 = mb_wm_object_ref (MB_WM_OBJECT (cclient_mainview));
          data->cclient2_actor = g_object_ref (
              mb_wm_comp_mgr_clutter_client_get_actor (cclient_mainview));
          mb_wm_comp_mgr_clutter_client_set_flags (cclient_mainview,
                                      MBWMCompMgrClutterClientDontUpdate |
                                      MBWMCompMgrClutterClientEffectRunning);
          HD_COMP_MGR_CLIENT (cclient_mainview)->effect = data;
        }
      return;
    }

  /* Need to store also pointer to the manager, as by the time
   * the effect finishes, the back pointer in the cm_client to
   * MBWindowManagerClient is not longer valid/set.
   */
  data = g_new0 (HDEffectData, 1);
  data->event = event;
  data->cclient = mb_wm_object_ref (MB_WM_OBJECT (cclient_subview));
  data->cclient_actor = g_object_ref (
      mb_wm_comp_mgr_clutter_client_get_actor( data->cclient ) );
  data->cclient2 = mb_wm_object_ref (MB_WM_OBJECT (cclient_mainview));
  data->cclient2_actor = g_object_ref (
      mb_wm_comp_mgr_clutter_client_get_actor( data->cclient2 ) );
  data->hmgr = HD_COMP_MGR (mgr);
  data->timeline =
        g_object_ref( hd_transition_timeline_new("subview", event, 250) );

  g_signal_connect (data->timeline, "new-frame",
                        G_CALLBACK (on_subview_timeline_new_frame), data);
  g_signal_connect (data->timeline, "completed",
                        G_CALLBACK (hd_transition_completed), data);

  mb_wm_comp_mgr_clutter_client_set_flags (cclient_subview,
                              MBWMCompMgrClutterClientDontUpdate |
                              MBWMCompMgrClutterClientEffectRunning);
  mb_wm_comp_mgr_clutter_client_set_flags (cclient_mainview,
                                MBWMCompMgrClutterClientDontUpdate |
                                MBWMCompMgrClutterClientEffectRunning);

  hd_comp_mgr_set_effect_running(mgr, TRUE);
  HD_COMP_MGR_CLIENT (cclient_mainview)->effect = data;
  HD_COMP_MGR_CLIENT (cclient_subview)->effect  = data;

  /* first call to stop flicker */
  on_subview_timeline_new_frame(data->timeline, 0, data);
  clutter_timeline_start (data->timeline);
}

/* Stop any currently active transition on the given client (assuming the
 * 'effect' member of the cclient has been set). Currently this is only done
 * for subview. */
void
hd_transition_stop(HdCompMgr                  *mgr,
                   MBWindowManagerClient      *client)
{
  MBWMCompMgrClutterClient * cclient;
  HDEffectData             * data;

  cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (client->cm_client);

  if ((data = HD_COMP_MGR_CLIENT (cclient)->effect))
    {
      gint n_frames = clutter_timeline_get_n_frames(data->timeline);
      clutter_timeline_stop(data->timeline);
      /* Make sure we update to the final state for this transition */
      g_signal_emit_by_name (data->timeline, "new-frame",
                             n_frames, NULL);
      /* Call end-of-transition handler */
      hd_transition_completed(data->timeline, data);
    }
}

/* Start or finish a transition for the rotation
 * (moving into/out of blanking depending on first_part)
 */
static void
hd_transition_fade_and_rotate(gboolean first_part,
                              gboolean goto_portrait,
                              GCallback finished_callback,
                              gpointer finished_callback_data)
{
  ClutterColor black = {0x00, 0x00, 0x00, 0xFF};
  HDEffectData *data = g_new0 (HDEffectData, 1);
  data->event = first_part ? MBWMCompMgrClientEventMap :
                             MBWMCompMgrClientEventUnmap;
  data->timeline =
        g_object_ref( hd_transition_timeline_new("rotate", data->event, 300) );

  g_signal_connect (data->timeline, "new-frame",
                        G_CALLBACK (on_rotate_screen_timeline_new_frame), data);
  g_signal_connect (data->timeline, "completed",
                         G_CALLBACK (hd_transition_completed), data);
  if (finished_callback)
    g_signal_connect_swapped (data->timeline, "completed",
                          G_CALLBACK (finished_callback), finished_callback_data);

  data->angle = hd_transition_get_double("rotate", "angle", 40);
  /* Set the direction of movement - we want to rotate backwards if we
   * go back to landscape as it looks better */
  if (first_part == goto_portrait)
    data->angle *= -1;
  /* Add the actor we use to dim out the screen */
  data->particles[0] = g_object_ref(clutter_rectangle_new_with_color(&black));
  clutter_actor_set_size(data->particles[0],
      hd_comp_mgr_get_current_screen_width (),
      hd_comp_mgr_get_current_screen_height ());
  clutter_container_add_actor(
            CLUTTER_CONTAINER(clutter_stage_get_default()),
            data->particles[0]);
  clutter_actor_show(data->particles[0]);
  if (!goto_portrait && first_part)
    {
      /* Add the actor we use to mask out the landscape part of the screen in the
       * portrait half of the animation. This is pretty nasty, but as the home
       * applets aren't repositioned they can sometimes be seen in the background.*/
       data->particles[1] = g_object_ref(clutter_rectangle_new_with_color(&black));
       clutter_actor_set_position(data->particles[1],
           HD_COMP_MGR_LANDSCAPE_HEIGHT, 0);
       clutter_actor_set_size(data->particles[1],
           HD_COMP_MGR_LANDSCAPE_WIDTH-HD_COMP_MGR_LANDSCAPE_HEIGHT,
           HD_COMP_MGR_LANDSCAPE_HEIGHT);
       clutter_container_add_actor(
                 CLUTTER_CONTAINER(hd_render_manager_get()),
                 data->particles[1]);
       clutter_actor_show(data->particles[1]);
    }

  /* stop flicker by calling the first frame directly */
  on_rotate_screen_timeline_new_frame(data->timeline, 0, data);
  clutter_timeline_start (data->timeline);
}

static gboolean
hd_transition_rotating_fsm(void)
{
  HDRMStateEnum state;
  gboolean change_state;

  g_debug ("%s: phase=%d, new_direction=%d, direction=%d", __FUNCTION__,
           Orientation_change.phase, Orientation_change.new_direction,
           Orientation_change.direction);

  /* We will always return FALSE, which will cancel the timeout,
   * so make sure it is set to 0. */
  Orientation_change.timeout_id = 0;
  /* if we enter here, we don't need the timer any more either */
  if (Orientation_change.timer) {
    g_timer_destroy(Orientation_change.timer);
    Orientation_change.timer = 0;
  }

  switch (Orientation_change.phase)
    {
      case IDLE:
        /* Fade to black ((c) Metallica) */
        Orientation_change.phase = FADE_OUT;
        Orientation_change.direction = Orientation_change.new_direction;
        hd_transition_fade_and_rotate(
                TRUE, Orientation_change.direction == GOTO_PORTRAIT,
                G_CALLBACK(hd_transition_rotating_fsm), NULL);
        break;
      case FADE_OUT:
        /*
         * We're faded out, now it is time to change HDRM state
         * if* requested and possible.  Take care not to switch
         * to states which don't support the orientation we're
         * going to.
         */
        state = Orientation_change.goto_state;
        change_state = Orientation_change.new_direction == GOTO_PORTRAIT
          ?  STATE_IS_PORTRAIT(state) || STATE_IS_PORTRAIT_CAPABLE(state)
          : !STATE_IS_PORTRAIT(state) && state != HDRM_STATE_UNDEFINED;
        if (change_state)
          {
            Orientation_change.goto_state = HDRM_STATE_UNDEFINED;
            hd_render_manager_set_state(state);
          }

        /* Now go through our list of waiting effects and complete them */
        while (Orientation_change.effects_waiting)
          {
            hd_transition_completed(0,
                (HDEffectData*)Orientation_change.effects_waiting->data);
            Orientation_change.effects_waiting =
              Orientation_change.effects_waiting->next;
          }
        g_list_free(Orientation_change.effects_waiting);
        Orientation_change.effects_waiting = 0;

        if (Orientation_change.direction == Orientation_change.new_direction)
          {
            /*
             * Wait for the screen change. During this period, blank the
             * screen by hiding %HdRenderManager. Note that we could wait
             * until redraws have finished here, but currently X blanks us
             * for a set time period anyway - and this way it is easier
             * to get rotation speeds sorted.
             */
            Orientation_change.phase = WAITING;
            clutter_actor_hide(CLUTTER_ACTOR(hd_render_manager_get()));
            hd_util_change_screen_orientation(Orientation_change.wm,
                         Orientation_change.direction == GOTO_PORTRAIT);
            Orientation_change.timeout_id =
              g_timeout_add(hd_transition_get_double("rotate", "damage_timeout", 50),
                            (GSourceFunc) hd_transition_rotating_fsm, NULL);
            Orientation_change.timer = g_timer_new();
            break;
          }
        else
          Orientation_change.direction = Orientation_change.new_direction;
        /* Fall through */
      case WAITING:
        if (Orientation_change.direction == Orientation_change.new_direction)
          { /* Fade back in */
            Orientation_change.phase = FADE_IN;
            clutter_actor_show(CLUTTER_ACTOR(hd_render_manager_get()));
            hd_transition_fade_and_rotate(
                    FALSE, Orientation_change.direction == GOTO_PORTRAIT,
                    G_CALLBACK(hd_transition_rotating_fsm), NULL);
            /* Fix NB#117109 by re-evaluating what is blurred and what isn't */
            hd_render_manager_restack();
          }
        else
          {
            Orientation_change.direction = Orientation_change.new_direction;
            Orientation_change.phase = FADE_OUT;
            hd_transition_rotating_fsm();
          }
        break;
      case FADE_IN:
        Orientation_change.phase = IDLE;
        if (Orientation_change.direction != Orientation_change.new_direction)
          hd_transition_rotating_fsm();
        break;
    }

  return FALSE;
}

/* Start changing the screen's orientation by rotating 90 degrees
 * (portrait mode) or going back to landscape.  Returns FALSE if
 * orientation changing won't take place. */
gboolean
hd_transition_rotate_screen (MBWindowManager *wm, gboolean goto_portrait)
{ g_debug("%s(goto_portrait=%d)", __FUNCTION__, goto_portrait);
  Orientation_change.wm = wm;
  Orientation_change.new_direction = goto_portrait
    ? GOTO_PORTRAIT : GOTO_LANDSCAPE;

  if (Orientation_change.phase == IDLE)
    {
      if (goto_portrait == hd_comp_mgr_is_portrait ())
        {
          g_warning("%s: already in %s mode", __FUNCTION__,
                    goto_portrait ? "portrait" : "landscape");
          return FALSE;
        }

      hd_transition_rotating_fsm();
    }
  else
    g_debug("divert");

  return TRUE;
}

/*
 * Asks the rotating machine to switch to @state if possible
 * when it's faded out.  We'll switch state with best effort,
 * but no promises.  Only effective if a rotation transition
 * is underway.
 */
void
hd_transition_rotate_screen_and_change_state (HDRMStateEnum state)
{
  Orientation_change.goto_state = state;
}

/* Returns whether we are in a state where we should ignore any
 * damage requests. This also checks and possibly prolongs how long
 * we stay in the WAITING state, so we can be sure that all windows
 * have updated before we fade back from black. */
gboolean
hd_transition_rotate_ignore_damage()
{
  if (Orientation_change.phase == WAITING)
    {
      /* Only postpone the timeout if we haven't postponed
       * it too long already. This stops us getting stuck
       * in the WAITING state if an app keeps redrawing. */
      if (g_timer_elapsed(Orientation_change.timer, NULL) <
          (hd_transition_get_double("rotate", "damage_timeout_max", 1000) / 1000.0))
        {
          /* Reset the timeout to be a little longer */
          if (Orientation_change.timeout_id)
            g_source_remove(Orientation_change.timeout_id);
          Orientation_change.timeout_id = g_timeout_add(
                hd_transition_get_double("rotate", "damage_timeout", 50),
                (GSourceFunc)hd_transition_rotating_fsm, NULL);
        }

      return TRUE;
    }
  return FALSE;
}

/* Returns whether @actor will last only as long as the effect
 * (if it has any) takes.  Currently only subview transitions
 * are considered. */
gboolean
hd_transition_actor_will_go_away (ClutterActor *actor)
{
  HdCompMgrClient *hcmgrc;

  if (!(hcmgrc = g_object_get_data(G_OBJECT(actor),
                                   "HD-MBWMCompMgrClutterClient")))
    return FALSE;
  if (!hcmgrc->effect || hcmgrc->effect->event != MBWMCompMgrClientEventUnmap)
    return FALSE;
  return hcmgrc->effect->cclient == MB_WM_COMP_MGR_CLUTTER_CLIENT (hcmgrc);
}

/* Start playing @fname asynchronously. */
void
hd_transition_play_sound (const gchar * fname)
{
    static ca_context *ca;
    ca_proplist *pl;
    int ret;
    GTimer *timer;
    gint millisec;

    /* Canberra uses threads. */
    if (hd_volume_profile_is_silent() || hd_disable_threads())
      return;

    /* Initialize the canberra context once. */
    if (!ca)
      {
        if ((ret = ca_context_create (&ca)) != CA_SUCCESS)
          {
            g_warning("ca_context_create: %s", ca_strerror (ret));
            return;
          }
        else if ((ret = ca_context_open (ca)) != CA_SUCCESS)
          {
            g_warning("ca_context_open: %s", ca_strerror (ret));
            ca_context_destroy(ca);
            ca = NULL;
            return;
          }
      }

    timer = g_timer_new();

    ca_proplist_create (&pl);
    ca_proplist_sets (pl, CA_PROP_CANBERRA_CACHE_CONTROL, "permanent");
    ca_proplist_sets (pl, CA_PROP_MEDIA_FILENAME, fname);
    ca_proplist_sets (pl, CA_PROP_MEDIA_ROLE, "event");
    if ((ret = ca_context_play_full (ca, 0, pl, NULL, NULL)) != CA_SUCCESS)
      g_warning("%s: %s", fname, ca_strerror (ret));
    ca_proplist_destroy(pl);
    millisec = (gint)(g_timer_elapsed(timer, 0)*1000);
    g_timer_destroy(timer);

    if (millisec > 100) /* [Bug 105635] */
      g_warning("%s: ca_context_play_full is blocking for %d ms to play %s",
          __FUNCTION__, millisec, fname);

}

static gboolean
transitions_ini_changed(GIOChannel *chnl, GIOCondition cond, gpointer unused)
{
  struct inotify_event ibuf;

  g_io_channel_read_chars(chnl, (void *)&ibuf, sizeof(ibuf), NULL, NULL);
  if (ibuf.mask & (IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
    {
      g_debug("disposing transitions.ini");
      transitions_ini_is_dirty = TRUE;

      /* Track no more if the dirent changed or disappeared. */
      if (ibuf.mask & (IN_MOVE_SELF | IN_DELETE_SELF | IN_IGNORED))
        {
          g_debug("watching no more");
          g_io_channel_unref(chnl);
          return FALSE;
        }
    }
  return TRUE;
}

static GKeyFile *
hd_transition_get_keyfile(void)
{
  static GKeyFile *transitions_ini;
  static GIOChannel *transitions_ini_watcher;
  GError *error;
  GKeyFile *ini;

  if (transitions_ini && !transitions_ini_is_dirty)
    return transitions_ini;
  g_debug("%s transitions.ini",
          transitions_ini_is_dirty ? "reloading" : "loading");

  error = NULL;
  ini = g_key_file_new();
  if (!g_key_file_load_from_file (ini, TRANSITIONS_INI, 0, &error))
    { /* Use the previous @transitions_ini. */
      g_warning("couldn't load %s: %s", TRANSITIONS_INI, error->message);
      if (!transitions_ini)
        g_warning("using default settings");
      g_error_free(error);
      g_key_file_free(ini);
      return transitions_ini;
    }

  /* Use the new @transitions_ini. */
  transitions_ini = ini;

  if (!transitions_ini_watcher)
    {
      static int inofd = -1, watch = -1;

      /* Create an inotify if we haven't. */
      if (inofd < 0 && (inofd = inotify_init()) < 0)
        {
          g_warning("inotify_init: %s", strerror(errno));
          goto out;
        }

      if (watch >= 0)
        /* Remove the previous watch. */
        inotify_rm_watch(inofd, watch);
      watch = inotify_add_watch(inofd, TRANSITIONS_INI,
                              IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF);
      if (watch < 0)
        {
          g_warning("inotify_add_watch: %s", strerror(errno));
          goto out;
        }

      transitions_ini_watcher = g_io_channel_unix_new(inofd);
      g_io_add_watch_full(transitions_ini_watcher, G_PRIORITY_DEFAULT, G_IO_IN,
                          transitions_ini_changed, &transitions_ini_watcher,
                          (GDestroyNotify)g_nullify_pointer);
      g_debug("watching transitions.ini");
    }

  /* Stop reloading @transitions_ini if we can watch it. */
  transitions_ini_is_dirty = FALSE;

out:
  return transitions_ini;
}

gint
hd_transition_get_int(const gchar *transition, const char *key,
                      gint default_val)
{
  gint value;
  GError *error;
  GKeyFile *ini;

  if (!(ini = hd_transition_get_keyfile()))
    return default_val;

  error = NULL;
  value = g_key_file_get_integer(ini, transition, key, &error);
  if (error)
    {
      g_warning("couldn't read %s::%s from transitions.ini: %s",
                transition, key, error->message);
      g_error_free(error);
      return default_val;
    }

  return value;
}

gdouble
hd_transition_get_double(const gchar *transition,
                         const char *key, gdouble default_val)
{
  gdouble value;
  GError *error;
  GKeyFile *ini;

  if (!(ini = hd_transition_get_keyfile()))
    return default_val;

  error = NULL;
  value = g_key_file_get_double (ini, transition, key, &error);
  if (error)
    {
      g_warning("couldn't read %s::%s from transitions.ini: %s",
                transition, key, error->message);
      g_error_free(error);
      return default_val;
    }

  return value;
}
