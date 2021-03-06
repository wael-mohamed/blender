/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 */

/** \file
 * \ingroup edinterface
 */

/* a full doc with API notes can be found in
 * bf-blender/trunk/blender/doc/guides/interface_API.txt */

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "PIL_time.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "BKE_context.h"
#include "BKE_screen.h"

#include "BLF_api.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_screen.h"

#include "UI_interface.h"
#include "UI_interface_icons.h"
#include "UI_resources.h"
#include "UI_view2d.h"

#include "GPU_batch_presets.h"
#include "GPU_immediate.h"
#include "GPU_matrix.h"
#include "GPU_state.h"

#include "interface_intern.h"

/* -------------------------------------------------------------------- */
/** \name Defines & Structs
 * \{ */

#define ANIMATION_TIME 0.30
#define ANIMATION_INTERVAL 0.02

typedef enum uiPanelRuntimeFlag {
  PANEL_LAST_ADDED = (1 << 0),
  PANEL_ACTIVE = (1 << 2),
  PANEL_WAS_ACTIVE = (1 << 3),
  PANEL_ANIM_ALIGN = (1 << 4),
  PANEL_NEW_ADDED = (1 << 5),
  PANEL_SEARCH_FILTER_MATCH = (1 << 7),
} uiPanelRuntimeFlag;

/* The state of the mouse position relative to the panel. */
typedef enum uiPanelMouseState {
  PANEL_MOUSE_OUTSIDE,        /** Mouse is not in the panel. */
  PANEL_MOUSE_INSIDE_CONTENT, /** Mouse is in the actual panel content. */
  PANEL_MOUSE_INSIDE_HEADER,  /** Mouse is in the panel header. */
} uiPanelMouseState;

typedef enum uiHandlePanelState {
  PANEL_STATE_DRAG,
  PANEL_STATE_DRAG_SCALE,
  PANEL_STATE_WAIT_UNTAB,
  PANEL_STATE_ANIMATION,
  PANEL_STATE_EXIT,
} uiHandlePanelState;

typedef struct uiHandlePanelData {
  uiHandlePanelState state;

  /* Animation. */
  wmTimer *animtimer;
  double starttime;

  /* Dragging. */
  bool is_drag_drop;
  int startx, starty;
  int startofsx, startofsy;
  int startsizex, startsizey;
  float start_cur_xmin, start_cur_ymin;
} uiHandlePanelData;

typedef struct PanelSort {
  Panel *panel;
  int new_offset_x;
  int new_offset_y;
} PanelSort;

static int get_panel_real_size_y(const Panel *panel);
static void panel_activate_state(const bContext *C, Panel *panel, uiHandlePanelState state);
static int compare_panel(const void *a, const void *b);
static bool panel_type_context_poll(ARegion *region,
                                    const PanelType *panel_type,
                                    const char *context);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Local Functions
 * \{ */

static void panel_title_color_get(const Panel *panel,
                                  const bool show_background,
                                  const bool use_search_color,
                                  const bool region_search_filter_active,
                                  uchar r_color[4])
{
  if (!show_background) {
    /* Use menu colors for floating panels. */
    bTheme *btheme = UI_GetTheme();
    const uiWidgetColors *wcol = &btheme->tui.wcol_menu_back;
    copy_v4_v4_uchar(r_color, (const uchar *)wcol->text);
    return;
  }

  const bool search_match = UI_panel_matches_search_filter(panel);

  if (region_search_filter_active && use_search_color && search_match) {
    UI_GetThemeColor4ubv(TH_MATCH, r_color);
  }
  else {
    UI_GetThemeColor4ubv(TH_TITLE, r_color);
    if (region_search_filter_active && !search_match) {
      r_color[0] *= 0.5;
      r_color[1] *= 0.5;
      r_color[2] *= 0.5;
    }
  }
}

static bool panel_active_animation_changed(ListBase *lb,
                                           Panel **r_panel_animation,
                                           bool *r_no_animation)
{
  LISTBASE_FOREACH (Panel *, panel, lb) {
    /* Detect panel active flag changes. */
    if (!(panel->type && panel->type->parent)) {
      if ((panel->runtime_flag & PANEL_WAS_ACTIVE) && !(panel->runtime_flag & PANEL_ACTIVE)) {
        return true;
      }
      if (!(panel->runtime_flag & PANEL_WAS_ACTIVE) && (panel->runtime_flag & PANEL_ACTIVE)) {
        return true;
      }
    }

    if ((panel->runtime_flag & PANEL_ACTIVE) && !(panel->flag & PNL_CLOSED)) {
      if (panel_active_animation_changed(&panel->children, r_panel_animation, r_no_animation)) {
        return true;
      }
    }

    /* Detect animation. */
    if (panel->activedata) {
      uiHandlePanelData *data = panel->activedata;
      if (data->state == PANEL_STATE_ANIMATION) {
        *r_panel_animation = panel;
      }
      else {
        /* Don't animate while handling other interaction. */
        *r_no_animation = true;
      }
    }
    if ((panel->runtime_flag & PANEL_ANIM_ALIGN) && !(*r_panel_animation)) {
      *r_panel_animation = panel;
    }
  }

  return false;
}

static bool panels_need_realign(ScrArea *area, ARegion *region, Panel **r_panel_animation)
{
  *r_panel_animation = NULL;

  if (area->spacetype == SPACE_PROPERTIES && region->regiontype == RGN_TYPE_WINDOW) {
    SpaceProperties *sbuts = area->spacedata.first;

    if (sbuts->mainbo != sbuts->mainb) {
      return true;
    }
  }
  else if (area->spacetype == SPACE_IMAGE && region->regiontype == RGN_TYPE_PREVIEW) {
    return true;
  }
  else if (area->spacetype == SPACE_FILE && region->regiontype == RGN_TYPE_CHANNELS) {
    return true;
  }

  /* Detect if a panel was added or removed. */
  Panel *panel_animation = NULL;
  bool no_animation = false;
  if (panel_active_animation_changed(&region->panels, &panel_animation, &no_animation)) {
    return true;
  }

  /* Detect panel marked for animation, if we're not already animating. */
  if (panel_animation) {
    if (!no_animation) {
      *r_panel_animation = panel_animation;
    }
    return true;
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Functions for Instanced Panels
 * \{ */

static Panel *UI_panel_add_instanced_ex(ARegion *region,
                                        ListBase *panels,
                                        PanelType *panel_type,
                                        PointerRNA *custom_data)
{
  Panel *panel = MEM_callocN(sizeof(Panel), "instanced panel");
  panel->type = panel_type;
  BLI_strncpy(panel->panelname, panel_type->idname, sizeof(panel->panelname));

  panel->runtime.custom_data_ptr = custom_data;
  panel->runtime_flag |= PANEL_NEW_ADDED;

  /* Add the panel's children too. Although they aren't instanced panels, we can still use this
   * function to create them, as UI_panel_begin does other things we don't need to do. */
  LISTBASE_FOREACH (LinkData *, child, &panel_type->children) {
    PanelType *child_type = child->data;
    UI_panel_add_instanced_ex(region, &panel->children, child_type, custom_data);
  }

  /* Make sure the panel is added to the end of the display-order as well. This is needed for
   * loading existing files.
   *
   * Note: We could use special behavior to place it after the panel that starts the list of
   * instanced panels, but that would add complexity that isn't needed for now. */
  int max_sortorder = 0;
  LISTBASE_FOREACH (Panel *, existing_panel, panels) {
    if (existing_panel->sortorder > max_sortorder) {
      max_sortorder = existing_panel->sortorder;
    }
  }
  panel->sortorder = max_sortorder + 1;

  BLI_addtail(panels, panel);

  return panel;
}

/**
 * Called in situations where panels need to be added dynamically rather than
 * having only one panel corresponding to each #PanelType.
 */
Panel *UI_panel_add_instanced(ARegion *region,
                              ListBase *panels,
                              char *panel_idname,
                              PointerRNA *custom_data)
{
  ARegionType *region_type = region->type;

  PanelType *panel_type = BLI_findstring(
      &region_type->paneltypes, panel_idname, offsetof(PanelType, idname));

  if (panel_type == NULL) {
    printf("Panel type '%s' not found.\n", panel_idname);
    return NULL;
  }

  return UI_panel_add_instanced_ex(region, panels, panel_type, custom_data);
}

/**
 * Find a unique key to append to the #PanelTyype.idname for the lookup to the panel's #uiBlock.
 * Needed for instanced panels, where there can be multiple with the same type and identifier.
 */
void UI_list_panel_unique_str(Panel *panel, char *r_name)
{
  /* The panel sortorder will be unique for a specific panel type because the instanced
   * panel list is regenerated for every change in the data order / length. */
  snprintf(r_name, INSTANCED_PANEL_UNIQUE_STR_LEN, "%d", panel->sortorder);
}

/**
 * Free a panel and it's children. Custom data is shared by the panel and its children
 * and is freed by #UI_panels_free_instanced.
 *
 * \note The only panels that should need to be deleted at runtime are panels with the
 * #PNL_INSTANCED flag set.
 */
static void panel_delete(const bContext *C, ARegion *region, ListBase *panels, Panel *panel)
{
  /* Recursively delete children. */
  LISTBASE_FOREACH_MUTABLE (Panel *, child, &panel->children) {
    panel_delete(C, region, &panel->children, child);
  }
  BLI_freelistN(&panel->children);

  BLI_remlink(panels, panel);
  if (panel->activedata) {
    MEM_freeN(panel->activedata);
  }
  MEM_freeN(panel);
}

/**
 * Remove instanced panels from the region's panel list.
 *
 * \note Can be called with NULL \a C, but it should be avoided because
 * handlers might not be removed.
 */
void UI_panels_free_instanced(const bContext *C, ARegion *region)
{
  /* Delete panels with the instanced flag. */
  LISTBASE_FOREACH_MUTABLE (Panel *, panel, &region->panels) {
    if ((panel->type != NULL) && (panel->type->flag & PNL_INSTANCED)) {
      /* Make sure the panel's handler is removed before deleting it. */
      if (C != NULL && panel->activedata != NULL) {
        panel_activate_state(C, panel, PANEL_STATE_EXIT);
      }

      /* Free panel's custom data. */
      if (panel->runtime.custom_data_ptr != NULL) {
        MEM_freeN(panel->runtime.custom_data_ptr);
      }

      /* Free the panel and its sub-panels. */
      panel_delete(C, region, &region->panels, panel);
    }
  }
}

/**
 * Check if the instanced panels in the region's panels correspond to the list of data the panels
 * represent. Returns false if the panels have been reordered or if the types from the list data
 * don't match in any way.
 *
 * \param data: The list of data to check against the instanced panels.
 * \param panel_idname_func: Function to find the #PanelType.idname for each item in the data list.
 * For a readability and generality, this lookup happens separately for each type of panel list.
 */
bool UI_panel_list_matches_data(ARegion *region,
                                ListBase *data,
                                uiListPanelIDFromDataFunc panel_idname_func)
{
  /* Check for NULL data. */
  int data_len = 0;
  Link *data_link = NULL;
  if (data == NULL) {
    data_len = 0;
    data_link = NULL;
  }
  else {
    data_len = BLI_listbase_count(data);
    data_link = data->first;
  }

  int i = 0;
  LISTBASE_FOREACH (Panel *, panel, &region->panels) {
    if (panel->type != NULL && panel->type->flag & PNL_INSTANCED) {
      /* The panels were reordered by drag and drop. */
      if (panel->flag & PNL_INSTANCED_LIST_ORDER_CHANGED) {
        return false;
      }

      /* We reached the last data item before the last instanced panel. */
      if (data_link == NULL) {
        return false;
      }

      /* Check if the panel type matches the panel type from the data item. */
      char panel_idname[MAX_NAME];
      panel_idname_func(data_link, panel_idname);
      if (!STREQ(panel_idname, panel->type->idname)) {
        return false;
      }

      data_link = data_link->next;
      i++;
    }
  }

  /* If we didn't make it to the last list item, the panel list isn't complete. */
  if (i != data_len) {
    return false;
  }

  return true;
}

static void reorder_instanced_panel_list(bContext *C, ARegion *region, Panel *drag_panel)
{
  /* Without a type we cannot access the reorder callback. */
  if (drag_panel->type == NULL) {
    return;
  }
  /* Don't reorder if this instanced panel doesn't support drag and drop reordering. */
  if (drag_panel->type->reorder == NULL) {
    return;
  }

  char *context = NULL;
  if (!UI_panel_category_is_visible(region)) {
    context = drag_panel->type->context;
  }

  /* Find how many instanced panels with this context string. */
  int list_panels_len = 0;
  LISTBASE_FOREACH (Panel *, panel, &region->panels) {
    if (panel->type) {
      if (panel->type->flag & PNL_INSTANCED) {
        if (panel_type_context_poll(region, panel->type, context)) {
          list_panels_len++;
        }
      }
    }
  }

  /* Sort the matching instanced panels by their display order. */
  PanelSort *panel_sort = MEM_callocN(list_panels_len * sizeof(*panel_sort), "instancedpanelsort");
  PanelSort *sort_index = panel_sort;
  LISTBASE_FOREACH (Panel *, panel, &region->panels) {
    if (panel->type) {
      if (panel->type->flag & PNL_INSTANCED) {
        if (panel_type_context_poll(region, panel->type, context)) {
          sort_index->panel = panel;
          sort_index++;
        }
      }
    }
  }
  qsort(panel_sort, list_panels_len, sizeof(*panel_sort), compare_panel);

  /* Find how many of those panels are above this panel. */
  int move_to_index = 0;
  for (; move_to_index < list_panels_len; move_to_index++) {
    if (panel_sort[move_to_index].panel == drag_panel) {
      break;
    }
  }

  MEM_freeN(panel_sort);

  /* Set the bit to tell the interface to instanced the list. */
  drag_panel->flag |= PNL_INSTANCED_LIST_ORDER_CHANGED;

  /* Finally, move this panel's list item to the new index in its list. */
  drag_panel->type->reorder(C, drag_panel, move_to_index);
}

/**
 * Recursive implementation for #UI_panel_set_expand_from_list_data.
 *
 * \return Whether the closed flag for the panel or any sub-panels changed.
 */
static bool panel_set_expand_from_list_data_recursive(Panel *panel, short flag, short *flag_index)
{
  const bool open = (flag & (1 << *flag_index));
  bool changed = (open == (bool)(panel->flag & PNL_CLOSED));
  SET_FLAG_FROM_TEST(panel->flag, !open, PNL_CLOSED);

  LISTBASE_FOREACH (Panel *, child, &panel->children) {
    *flag_index = *flag_index + 1;
    changed |= panel_set_expand_from_list_data_recursive(child, flag, flag_index);
  }
  return changed;
}

/**
 * Set the expansion of the panel and its sub-panels from the flag stored by the list data
 * corresponding to this panel. The flag has expansion stored in each bit in depth first
 * order.
 */
void UI_panel_set_expand_from_list_data(const bContext *C, Panel *panel)
{
  BLI_assert(panel->type != NULL);
  BLI_assert(panel->type->flag & PNL_INSTANCED);
  if (panel->type->get_list_data_expand_flag == NULL) {
    /* Instanced panel doesn't support loading expansion. */
    return;
  }

  const short expand_flag = panel->type->get_list_data_expand_flag(C, panel);
  short flag_index = 0;

  /* Start panel animation if the open state was changed. */
  if (panel_set_expand_from_list_data_recursive(panel, expand_flag, &flag_index)) {
    panel_activate_state(C, panel, PANEL_STATE_ANIMATION);
  }
}

/**
 * Set expansion based on the data for instanced panels.
 */
static void region_panels_set_expansion_from_list_data(const bContext *C, ARegion *region)
{
  LISTBASE_FOREACH (Panel *, panel, &region->panels) {
    PanelType *panel_type = panel->type;
    if (panel_type != NULL && panel->type->flag & PNL_INSTANCED) {
      UI_panel_set_expand_from_list_data(C, panel);
    }
  }
}

/**
 * Recursive implementation for #set_panels_list_data_expand_flag.
 */
static void get_panel_expand_flag(Panel *panel, short *flag, short *flag_index)
{
  const bool open = !(panel->flag & PNL_CLOSED);
  SET_FLAG_FROM_TEST(*flag, open, (1 << *flag_index));

  LISTBASE_FOREACH (Panel *, child, &panel->children) {
    *flag_index = *flag_index + 1;
    get_panel_expand_flag(child, flag, flag_index);
  }
}

/**
 * Call the callback to store the panel and sub-panel expansion settings in the list item that
 * corresponds to each instanced panel.
 *
 * \note This needs to iterate through all of the regions panels because the panel with changed
 * expansion could have been the sub-panel of a instanced panel, meaning it might not know
 * which list item it corresponds to.
 */
static void set_panels_list_data_expand_flag(const bContext *C, const ARegion *region)
{
  LISTBASE_FOREACH (Panel *, panel, &region->panels) {
    PanelType *panel_type = panel->type;
    if (panel_type == NULL) {
      continue;
    }

    /* Check for #PANEL_ACTIVE so we only set the expand flag for active panels. */
    if (panel_type->flag & PNL_INSTANCED && panel->runtime_flag & PANEL_ACTIVE) {
      short expand_flag;
      short flag_index = 0;
      get_panel_expand_flag(panel, &expand_flag, &flag_index);
      if (panel->type->set_list_data_expand_flag) {
        panel->type->set_list_data_expand_flag(C, panel, expand_flag);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Panels
 * \{ */

/**
 * Set flag state for a panel and its sub-panels.
 *
 * \return True if this function changed any of the flags, false if it didn't.
 */
static bool panel_set_flag_recursive(Panel *panel, int flag, bool value)
{
  const short flag_original = panel->flag;

  SET_FLAG_FROM_TEST(panel->flag, value, flag);

  bool changed = (flag_original != panel->flag);

  LISTBASE_FOREACH (Panel *, child, &panel->children) {
    changed |= panel_set_flag_recursive(child, flag, value);
  }

  return changed;
}

static void panels_collapse_all(ARegion *region, const Panel *from_panel)
{
  const bool has_category_tabs = UI_panel_category_is_visible(region);
  const char *category = has_category_tabs ? UI_panel_category_active_get(region, false) : NULL;
  const PanelType *from_pt = from_panel->type;

  LISTBASE_FOREACH (Panel *, panel, &region->panels) {
    PanelType *pt = panel->type;

    /* Close panels with headers in the same context. */
    if (pt && from_pt && !(pt->flag & PNL_NO_HEADER)) {
      if (!pt->context[0] || !from_pt->context[0] || STREQ(pt->context, from_pt->context)) {
        if ((panel->flag & PNL_PIN) || !category || !pt->category[0] ||
            STREQ(pt->category, category)) {
          panel->flag |= PNL_CLOSED;
        }
      }
    }
  }
}

static bool panel_type_context_poll(ARegion *region,
                                    const PanelType *panel_type,
                                    const char *context)
{
  if (UI_panel_category_is_visible(region)) {
    return STREQ(panel_type->category, UI_panel_category_active_get(region, false));
  }

  if (panel_type->context[0] && STREQ(panel_type->context, context)) {
    return true;
  }

  return false;
}

Panel *UI_panel_find_by_type(ListBase *lb, PanelType *pt)
{
  const char *idname = pt->idname;

  LISTBASE_FOREACH (Panel *, panel, lb) {
    if (STREQLEN(panel->panelname, idname, sizeof(panel->panelname))) {
      return panel;
    }
  }
  return NULL;
}

/**
 * \note \a panel should be return value from #UI_panel_find_by_type and can be NULL.
 */
Panel *UI_panel_begin(
    ARegion *region, ListBase *lb, uiBlock *block, PanelType *pt, Panel *panel, bool *r_open)
{
  Panel *panel_last;
  const char *drawname = CTX_IFACE_(pt->translation_context, pt->label);
  const char *idname = pt->idname;
  const bool newpanel = (panel == NULL);

  if (newpanel) {
    panel = MEM_callocN(sizeof(Panel), "new panel");
    panel->type = pt;
    BLI_strncpy(panel->panelname, idname, sizeof(panel->panelname));

    if (pt->flag & PNL_DEFAULT_CLOSED) {
      panel->flag |= PNL_CLOSED;
    }

    panel->ofsx = 0;
    panel->ofsy = 0;
    panel->sizex = 0;
    panel->sizey = 0;
    panel->blocksizex = 0;
    panel->blocksizey = 0;
    panel->runtime_flag |= PANEL_NEW_ADDED;

    BLI_addtail(lb, panel);
  }
  else {
    /* Panel already exists. */
    panel->type = pt;
  }

  /* Do not allow closed panels without headers! Else user could get "disappeared" UI! */
  if ((pt->flag & PNL_NO_HEADER) && (panel->flag & PNL_CLOSED)) {
    panel->flag &= ~PNL_CLOSED;
    /* Force update of panels' positions. */
    panel->sizex = 0;
    panel->sizey = 0;
    panel->blocksizex = 0;
    panel->blocksizey = 0;
  }

  BLI_strncpy(panel->drawname, drawname, sizeof(panel->drawname));

  /* If a new panel is added, we insert it right after the panel that was last added.
   * This way new panels are inserted in the right place between versions. */
  for (panel_last = lb->first; panel_last; panel_last = panel_last->next) {
    if (panel_last->runtime_flag & PANEL_LAST_ADDED) {
      BLI_remlink(lb, panel);
      BLI_insertlinkafter(lb, panel_last, panel);
      break;
    }
  }

  if (newpanel) {
    panel->sortorder = (panel_last) ? panel_last->sortorder + 1 : 0;

    LISTBASE_FOREACH (Panel *, panel_next, lb) {
      if (panel_next != panel && panel_next->sortorder >= panel->sortorder) {
        panel_next->sortorder++;
      }
    }
  }

  if (panel_last) {
    panel_last->runtime_flag &= ~PANEL_LAST_ADDED;
  }

  /* Assign the new panel to the block. */
  block->panel = panel;
  panel->runtime_flag |= PANEL_ACTIVE | PANEL_LAST_ADDED;
  if (region->alignment == RGN_ALIGN_FLOAT) {
    UI_block_theme_style_set(block, UI_BLOCK_THEME_STYLE_POPUP);
  }

  *r_open = false;

  if (panel->flag & PNL_CLOSED) {
    return panel;
  }

  *r_open = true;

  return panel;
}

static float panel_region_offset_x_get(const ARegion *region)
{
  if (UI_panel_category_is_visible(region)) {
    if (RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT) {
      return UI_PANEL_CATEGORY_MARGIN_WIDTH;
    }
  }

  return 0;
}

void UI_panel_end(const ARegion *region, uiBlock *block, int width, int height, bool open)
{
  Panel *panel = block->panel;

  /* Set panel size excluding children. */
  panel->blocksizex = width;
  panel->blocksizey = height;

  /* Compute total panel size including children. */
  LISTBASE_FOREACH (Panel *, pachild, &panel->children) {
    if (pachild->runtime_flag & PANEL_ACTIVE) {
      width = max_ii(width, pachild->sizex);
      height += get_panel_real_size_y(pachild);
    }
  }

  /* Update total panel size. */
  if (panel->runtime_flag & PANEL_NEW_ADDED) {
    panel->runtime_flag &= ~PANEL_NEW_ADDED;
    panel->sizex = width;
    panel->sizey = height;
  }
  else {
    const int old_sizex = panel->sizex, old_sizey = panel->sizey;
    const int old_region_ofsx = panel->runtime.region_ofsx;

    /* Update width/height if non-zero. */
    if (width != 0) {
      panel->sizex = width;
    }
    if (height != 0 || open) {
      panel->sizey = height;
    }

    /* Check if we need to do an animation. */
    if (panel->sizex != old_sizex || panel->sizey != old_sizey) {
      panel->runtime_flag |= PANEL_ANIM_ALIGN;
      panel->ofsy += old_sizey - panel->sizey;
    }

    panel->runtime.region_ofsx = panel_region_offset_x_get(region);
    if (old_region_ofsx != panel->runtime.region_ofsx) {
      panel->runtime_flag |= PANEL_ANIM_ALIGN;
    }
  }
}

static void ui_offset_panel_block(uiBlock *block)
{
  const uiStyle *style = UI_style_get_dpi();

  /* Compute bounds and offset. */
  ui_block_bounds_calc(block);

  const int ofsy = block->panel->sizey - style->panelspace;

  LISTBASE_FOREACH (uiBut *, but, &block->buttons) {
    but->rect.ymin += ofsy;
    but->rect.ymax += ofsy;
  }

  block->rect.xmax = block->panel->sizex;
  block->rect.ymax = block->panel->sizey;
  block->rect.xmin = block->rect.ymin = 0.0;
}

void ui_panel_tag_search_filter_match(struct Panel *panel)
{
  panel->runtime_flag |= PANEL_SEARCH_FILTER_MATCH;
}

static void panel_matches_search_filter_recursive(const Panel *panel, bool *filter_matches)
{
  *filter_matches |= panel->runtime_flag & PANEL_SEARCH_FILTER_MATCH;

  /* If the panel has no match we need to make sure that its children are too. */
  if (!*filter_matches) {
    LISTBASE_FOREACH (const Panel *, child_panel, &panel->children) {
      panel_matches_search_filter_recursive(child_panel, filter_matches);
    }
  }
}

/**
 * Find whether a panel or any of its sub-panels contain a property that matches the search filter,
 * depending on the search process running in #UI_block_apply_search_filter earlier.
 */
bool UI_panel_matches_search_filter(const Panel *panel)
{
  bool search_filter_matches = false;
  panel_matches_search_filter_recursive(panel, &search_filter_matches);
  return search_filter_matches;
}

/**
 * Expands a panel if it was tagged as having a result by property search, otherwise collapses it.
 */
static void panel_set_expansion_from_seach_filter_recursive(const bContext *C, Panel *panel)
{
  short start_flag = panel->flag;
  SET_FLAG_FROM_TEST(panel->flag, !UI_panel_matches_search_filter(panel), PNL_CLOSED);
  if (start_flag != panel->flag) {
    panel_activate_state(C, panel, PANEL_STATE_ANIMATION);
  }

  /* If the panel is filtered (removed) we need to check that its children are too. */
  LISTBASE_FOREACH (Panel *, child_panel, &panel->children) {
    if (panel->runtime_flag & PANEL_ACTIVE) {
      if (!(panel->type->flag & PNL_NO_HEADER)) {
        panel_set_expansion_from_seach_filter_recursive(C, child_panel);
      }
    }
  }
}

/**
 * Uses the panel's search filter flag to set its expansion, activating animation if it was closed
 * or opened. Note that this can't be set too often, or manual interaction becomes impossible.
 */
void UI_panels_set_expansion_from_seach_filter(const bContext *C, ARegion *region)
{
  LISTBASE_FOREACH (Panel *, panel, &region->panels) {
    if (panel->runtime_flag & PANEL_ACTIVE) {
      if (!(panel->type->flag & PNL_NO_HEADER)) {
        panel_set_expansion_from_seach_filter_recursive(C, panel);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

/* Triangle 'icon' for panel header. */
void UI_draw_icon_tri(float x, float y, char dir, const float color[4])
{
  const float f3 = 0.05 * U.widget_unit;
  const float f5 = 0.15 * U.widget_unit;
  const float f7 = 0.25 * U.widget_unit;

  if (dir == 'h') {
    UI_draw_anti_tria(x - f3, y - f5, x - f3, y + f5, x + f7, y, color);
  }
  else if (dir == 't') {
    UI_draw_anti_tria(x - f5, y - f7, x + f5, y - f7, x, y + f3, color);
  }
  else { /* 'v' = vertical, down. */
    UI_draw_anti_tria(x - f5, y + f3, x + f5, y + f3, x, y - f7, color);
  }
}

#define PNL_ICON UI_UNIT_X /* Could be UI_UNIT_Y too. */

/* For button layout next to label. */
void UI_panel_label_offset(uiBlock *block, int *r_x, int *r_y)
{
  Panel *panel = block->panel;
  const bool is_subpanel = (panel->type && panel->type->parent);

  *r_x = UI_UNIT_X * 1.0f;
  *r_y = UI_UNIT_Y * 1.5f;

  if (is_subpanel) {
    *r_x += (0.7f * UI_UNIT_X);
  }
}

static void ui_draw_aligned_panel_header(const uiStyle *style,
                                         const uiBlock *block,
                                         const rcti *rect,
                                         const bool show_background,
                                         const bool region_search_filter_active)
{
  const Panel *panel = block->panel;
  const bool is_subpanel = (panel->type && panel->type->parent);
  const uiFontStyle *fontstyle = (is_subpanel) ? &style->widgetlabel : &style->paneltitle;

  /* + 0.001f to avoid flirting with float inaccuracy .*/
  const int pnl_icons = (panel->labelofs + (1.1f * PNL_ICON)) / block->aspect + 0.001f;

  /* Draw text labels. */
  uchar col_title[4];
  panel_title_color_get(
      panel, show_background, is_subpanel, region_search_filter_active, col_title);
  col_title[3] = 255;

  rcti hrect = *rect;
  hrect.xmin = rect->xmin + pnl_icons;
  hrect.ymin -= 2.0f / block->aspect;
  UI_fontstyle_draw(fontstyle,
                    &hrect,
                    panel->drawname,
                    col_title,
                    &(struct uiFontStyleDraw_Params){
                        .align = UI_STYLE_TEXT_LEFT,
                    });
}

/**
 * Draw a panel integrated in buttons-window, tool/property lists etc.
 */
void ui_draw_aligned_panel(const uiStyle *style,
                           const uiBlock *block,
                           const rcti *rect,
                           const bool show_pin,
                           const bool show_background,
                           const bool region_search_filter_active)
{
  const Panel *panel = block->panel;
  float color[4];
  const bool is_subpanel = (panel->type && panel->type->parent);
  const bool show_drag = (!is_subpanel &&
                          /* FIXME(campbell): currently no background means floating panel which
                           * can't be dragged. This may be changed in future. */
                          show_background);
  const int panel_col = is_subpanel ? TH_PANEL_SUB_BACK : TH_PANEL_BACK;
  const bool draw_box_style = (panel->type && panel->type->flag & PNL_DRAW_BOX);

  /* Use the theme for box widgets for box-style panels. */
  uiWidgetColors *box_wcol = NULL;
  if (draw_box_style) {
    bTheme *btheme = UI_GetTheme();
    box_wcol = &btheme->tui.wcol_box;
  }

  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

  if (panel->type && (panel->type->flag & PNL_NO_HEADER)) {
    if (show_background) {
      immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
      immUniformThemeColor(panel_col);
      immRectf(pos, rect->xmin, rect->ymin, rect->xmax, rect->ymax);
      immUnbindProgram();
    }
    return;
  }

  /* Calculate header rectangle with + 0.001f to prevent flicker due to float inaccuracy. */
  rcti headrect = {
      rect->xmin, rect->xmax, rect->ymax, rect->ymax + floor(PNL_HEADER / block->aspect + 0.001f)};

  /* Draw a panel and header backdrops with an opaque box backdrop for box style panels. */
  if (draw_box_style && !is_subpanel) {
    /* Expand the top a tiny bit to give header buttons equal size above and below. */
    rcti box_rect = {rect->xmin,
                     rect->xmax,
                     (panel->flag & PNL_CLOSED) ? headrect.ymin : rect->ymin,
                     headrect.ymax + U.pixelsize};
    ui_draw_box_opaque(&box_rect, UI_CNR_ALL);

    /* Mimic the border between aligned box widgets for the bottom of the header. */
    if (!(panel->flag & PNL_CLOSED)) {
      immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
      GPU_blend(GPU_BLEND_ALPHA);

      immUniformColor4ubv(box_wcol->outline);
      immRectf(pos, rect->xmin, headrect.ymin - U.pixelsize, rect->xmax, headrect.ymin);
      uchar emboss_col[4];
      UI_GetThemeColor4ubv(TH_WIDGET_EMBOSS, emboss_col);
      immUniformColor4ubv(emboss_col);
      immRectf(pos,
               rect->xmin,
               headrect.ymin - U.pixelsize,
               rect->xmax,
               headrect.ymin - U.pixelsize - 1);

      GPU_blend(GPU_BLEND_NONE);
      immUnbindProgram();
    }
  }

  /* Draw the header backdrop. */
  if (show_background && !is_subpanel && !draw_box_style) {
    const float minx = rect->xmin;
    const float y = headrect.ymax;

    immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
    GPU_blend(GPU_BLEND_ALPHA);

    /* Draw with background color. */
    immUniformThemeColor(UI_panel_matches_search_filter(panel) ? TH_MATCH : TH_PANEL_HEADER);
    immRectf(pos, minx, headrect.ymin, rect->xmax, y);

    immBegin(GPU_PRIM_LINES, 4);

    immVertex2f(pos, minx, y);
    immVertex2f(pos, rect->xmax, y);

    immVertex2f(pos, minx, y);
    immVertex2f(pos, rect->xmax, y);

    immEnd();

    GPU_blend(GPU_BLEND_NONE);
    immUnbindProgram();
  }

  /* draw optional pin icon */
  if (show_pin && (block->panel->flag & PNL_PIN)) {
    uchar col_title[4];
    panel_title_color_get(panel, show_background, false, region_search_filter_active, col_title);

    GPU_blend(GPU_BLEND_ALPHA);
    UI_icon_draw_ex(headrect.xmax - ((PNL_ICON * 2.2f) / block->aspect),
                    headrect.ymin + (5.0f / block->aspect),
                    (panel->flag & PNL_PIN) ? ICON_PINNED : ICON_UNPINNED,
                    (block->aspect * U.inv_dpi_fac),
                    1.0f,
                    0.0f,
                    col_title,
                    false);
    GPU_blend(GPU_BLEND_NONE);
  }

  /* Draw the title. */
  rcti titlerect = headrect;
  if (is_subpanel) {
    titlerect.xmin += (0.7f * UI_UNIT_X) / block->aspect + 0.001f;
  }
  ui_draw_aligned_panel_header(
      style, block, &titlerect, show_background, region_search_filter_active);

  if (show_drag) {
    /* Make `itemrect` smaller. */
    const float scale = 0.7;
    rctf itemrect;
    itemrect.xmax = headrect.xmax - (0.2f * UI_UNIT_X);
    itemrect.xmin = itemrect.xmax - BLI_rcti_size_y(&headrect);
    itemrect.ymin = headrect.ymin;
    itemrect.ymax = headrect.ymax;
    BLI_rctf_scale(&itemrect, scale);

    GPU_matrix_push();
    GPU_matrix_translate_2f(itemrect.xmin, itemrect.ymin);

    const int col_tint = 84;
    float col_high[4], col_dark[4];
    UI_GetThemeColorShade4fv(TH_PANEL_HEADER, col_tint, col_high);
    UI_GetThemeColorShade4fv(TH_PANEL_BACK, -col_tint, col_dark);

    GPUBatch *batch = GPU_batch_preset_panel_drag_widget(
        U.pixelsize, col_high, col_dark, BLI_rcti_size_y(&headrect) * scale);
    GPU_batch_program_set_builtin(batch, GPU_SHADER_2D_FLAT_COLOR);
    GPU_batch_draw(batch);
    GPU_matrix_pop();
  }

  /* Draw panel backdrop. */
  if (!(panel->flag & PNL_CLOSED)) {
    /* in some occasions, draw a border */
    if (panel->flag & PNL_SELECT && !is_subpanel) {
      float radius;
      if (draw_box_style) {
        UI_draw_roundbox_corner_set(UI_CNR_ALL);
        radius = box_wcol->roundness * U.widget_unit;
      }
      else {
        UI_draw_roundbox_corner_set(UI_CNR_NONE);
        radius = 0.0f;
      }

      UI_GetThemeColorShade4fv(TH_BACK, -120, color);
      UI_draw_roundbox_aa(false,
                          0.5f + rect->xmin,
                          0.5f + rect->ymin,
                          0.5f + rect->xmax,
                          0.5f + headrect.ymax + 1,
                          radius,
                          color);
    }

    immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
    GPU_blend(GPU_BLEND_ALPHA);

    /* Draw panel backdrop if it wasn't already been drawn by the single opaque round box earlier.
     * Note: Sub-panels blend with panels, so they can't be opaque. */
    if (show_background && !(draw_box_style && !is_subpanel)) {
      /* Draw the bottom sub-panels. */
      if (draw_box_style) {
        if (panel->next) {
          immUniformThemeColor(panel_col);
          immRectf(
              pos, rect->xmin + U.pixelsize, rect->ymin, rect->xmax - U.pixelsize, rect->ymax);
        }
        else {
          /* Change the width a little bit to line up with sides. */
          UI_draw_roundbox_corner_set(UI_CNR_BOTTOM_RIGHT | UI_CNR_BOTTOM_LEFT);
          UI_GetThemeColor4fv(panel_col, color);
          UI_draw_roundbox_aa(true,
                              rect->xmin + U.pixelsize,
                              rect->ymin + U.pixelsize,
                              rect->xmax - U.pixelsize,
                              rect->ymax,
                              box_wcol->roundness * U.widget_unit,
                              color);
        }
      }
      else {
        immUniformThemeColor(panel_col);
        immRectf(pos, rect->xmin, rect->ymin, rect->xmax, rect->ymax);
      }
    }

    immUnbindProgram();
  }

  /* Draw collapse icon. */
  {
    rctf itemrect = {.xmin = titlerect.xmin,
                     .xmax = itemrect.xmin + BLI_rcti_size_y(&titlerect),
                     .ymin = titlerect.ymin,
                     .ymax = titlerect.ymax};
    BLI_rctf_scale(&itemrect, 0.25f);

    uchar col_title[4];
    panel_title_color_get(panel, show_background, false, region_search_filter_active, col_title);
    float tria_color[4];
    rgb_uchar_to_float(tria_color, col_title);
    tria_color[3] = 1.0f;

    if (panel->flag & PNL_CLOSED) {
      ui_draw_anti_tria_rect(&itemrect, 'h', tria_color);
    }
    else {
      ui_draw_anti_tria_rect(&itemrect, 'v', tria_color);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Drawing (Tabs)
 * \{ */

static void imm_buf_append(
    float vbuf[][2], uchar cbuf[][3], float x, float y, const uchar col[3], int *index)
{
  ARRAY_SET_ITEMS(vbuf[*index], x, y);
  ARRAY_SET_ITEMS(cbuf[*index], UNPACK3(col));
  (*index)++;
}

/* Based on UI_draw_roundbox, check on making a version which allows us to skip some sides. */
static void ui_panel_category_draw_tab(bool filled,
                                       float minx,
                                       float miny,
                                       float maxx,
                                       float maxy,
                                       float rad,
                                       const int roundboxtype,
                                       const bool use_highlight,
                                       const bool use_shadow,
                                       const bool use_flip_x,
                                       const uchar highlight_fade[3],
                                       const uchar col[3])
{
  float vec[4][2] = {{0.195, 0.02}, {0.55, 0.169}, {0.831, 0.45}, {0.98, 0.805}};

  for (int a = 0; a < 4; a++) {
    mul_v2_fl(vec[a], rad);
  }

  uint vert_len = 0;
  if (use_highlight) {
    vert_len += (roundboxtype & UI_CNR_TOP_RIGHT) ? 6 : 1;
    vert_len += (roundboxtype & UI_CNR_TOP_LEFT) ? 6 : 1;
  }
  if (use_highlight && !use_shadow) {
    vert_len++;
  }
  else {
    vert_len += (roundboxtype & UI_CNR_BOTTOM_RIGHT) ? 6 : 1;
    vert_len += (roundboxtype & UI_CNR_BOTTOM_LEFT) ? 6 : 1;
  }
  /* Maximum size. */
  float vbuf[24][2];
  uchar cbuf[24][3];
  int buf_index = 0;

  /* Start right-top corner. */
  if (use_highlight) {
    if (roundboxtype & UI_CNR_TOP_RIGHT) {
      imm_buf_append(vbuf, cbuf, maxx, maxy - rad, col, &buf_index);
      for (int a = 0; a < 4; a++) {
        imm_buf_append(vbuf, cbuf, maxx - vec[a][1], maxy - rad + vec[a][0], col, &buf_index);
      }
      imm_buf_append(vbuf, cbuf, maxx - rad, maxy, col, &buf_index);
    }
    else {
      imm_buf_append(vbuf, cbuf, maxx, maxy, col, &buf_index);
    }

    /* Left top-corner. */
    if (roundboxtype & UI_CNR_TOP_LEFT) {
      imm_buf_append(vbuf, cbuf, minx + rad, maxy, col, &buf_index);
      for (int a = 0; a < 4; a++) {
        imm_buf_append(vbuf, cbuf, minx + rad - vec[a][0], maxy - vec[a][1], col, &buf_index);
      }
      imm_buf_append(vbuf, cbuf, minx, maxy - rad, col, &buf_index);
    }
    else {
      imm_buf_append(vbuf, cbuf, minx, maxy, col, &buf_index);
    }
  }

  if (use_highlight && !use_shadow) {
    imm_buf_append(
        vbuf, cbuf, minx, miny + rad, highlight_fade ? col : highlight_fade, &buf_index);
  }
  else {
    /* Left bottom-corner. */
    if (roundboxtype & UI_CNR_BOTTOM_LEFT) {
      imm_buf_append(vbuf, cbuf, minx, miny + rad, col, &buf_index);
      for (int a = 0; a < 4; a++) {
        imm_buf_append(vbuf, cbuf, minx + vec[a][1], miny + rad - vec[a][0], col, &buf_index);
      }
      imm_buf_append(vbuf, cbuf, minx + rad, miny, col, &buf_index);
    }
    else {
      imm_buf_append(vbuf, cbuf, minx, miny, col, &buf_index);
    }

    /* Right-bottom corner. */
    if (roundboxtype & UI_CNR_BOTTOM_RIGHT) {
      imm_buf_append(vbuf, cbuf, maxx - rad, miny, col, &buf_index);
      for (int a = 0; a < 4; a++) {
        imm_buf_append(vbuf, cbuf, maxx - rad + vec[a][0], miny + vec[a][1], col, &buf_index);
      }
      imm_buf_append(vbuf, cbuf, maxx, miny + rad, col, &buf_index);
    }
    else {
      imm_buf_append(vbuf, cbuf, maxx, miny, col, &buf_index);
    }
  }

  if (use_flip_x) {
    const float midx = (minx + maxx) / 2.0f;
    for (int i = 0; i < buf_index; i++) {
      vbuf[i][0] = midx - (vbuf[i][0] - midx);
    }
  }

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  uint color = GPU_vertformat_attr_add(
      format, "color", GPU_COMP_U8, 3, GPU_FETCH_INT_TO_FLOAT_UNIT);

  immBindBuiltinProgram(GPU_SHADER_2D_SMOOTH_COLOR);
  immBegin(filled ? GPU_PRIM_TRI_FAN : GPU_PRIM_LINE_STRIP, vert_len);
  for (int i = 0; i < buf_index; i++) {
    immAttr3ubv(color, cbuf[i]);
    immVertex2fv(pos, vbuf[i]);
  }
  immEnd();
  immUnbindProgram();
}

/**
 * Draw vertical tabs on the left side of the region, one tab per category.
 */
void UI_panel_category_draw_all(ARegion *region, const char *category_id_active)
{
  // #define USE_FLAT_INACTIVE
  const bool is_left = RGN_ALIGN_ENUM_FROM_MASK(region->alignment != RGN_ALIGN_RIGHT);
  View2D *v2d = &region->v2d;
  const uiStyle *style = UI_style_get();
  const uiFontStyle *fstyle = &style->widget;
  const int fontid = fstyle->uifont_id;
  short fstyle_points = fstyle->points;
  const float aspect = ((uiBlock *)region->uiblocks.first)->aspect;
  const float zoom = 1.0f / aspect;
  const int px = max_ii(1, round_fl_to_int(U.pixelsize));
  const int px_x_sign = is_left ? px : -px;
  const int category_tabs_width = round_fl_to_int(UI_PANEL_CATEGORY_MARGIN_WIDTH * zoom);
  const float dpi_fac = UI_DPI_FAC;
  /* Padding of tabs around text. */
  const int tab_v_pad_text = round_fl_to_int((2 + ((px * 3) * dpi_fac)) * zoom);
  /* Padding between tabs. */
  const int tab_v_pad = round_fl_to_int((4 + (2 * px * dpi_fac)) * zoom);
  const float tab_curve_radius = ((px * 3) * dpi_fac) * zoom;
  /* We flip the tab drawing, so always use these flags. */
  const int roundboxtype = UI_CNR_TOP_LEFT | UI_CNR_BOTTOM_LEFT;
  bool is_alpha;
  bool do_scaletabs = false;
#ifdef USE_FLAT_INACTIVE
  bool is_active_prev = false;
#endif
  float scaletabs = 1.0f;
  /* Same for all tabs. */
  /* Intentionally don't scale by 'px'. */
  const int rct_xmin = is_left ? v2d->mask.xmin + 3 : (v2d->mask.xmax - category_tabs_width);
  const int rct_xmax = is_left ? v2d->mask.xmin + category_tabs_width : (v2d->mask.xmax - 3);
  const int text_v_ofs = (rct_xmax - rct_xmin) * 0.3f;

  int y_ofs = tab_v_pad;

  /* Primary theme colors. */
  uchar theme_col_back[4];
  uchar theme_col_text[3];
  uchar theme_col_text_hi[3];

  /* Tab colors. */
  uchar theme_col_tab_bg[4];
  uchar theme_col_tab_active[3];
  uchar theme_col_tab_inactive[3];

  /* Secondary theme colors. */
  uchar theme_col_tab_outline[3];
  uchar theme_col_tab_divider[3]; /* Line that divides tabs from the main region. */
  uchar theme_col_tab_highlight[3];
  uchar theme_col_tab_highlight_inactive[3];

  UI_GetThemeColor4ubv(TH_BACK, theme_col_back);
  UI_GetThemeColor3ubv(TH_TEXT, theme_col_text);
  UI_GetThemeColor3ubv(TH_TEXT_HI, theme_col_text_hi);

  UI_GetThemeColor4ubv(TH_TAB_BACK, theme_col_tab_bg);
  UI_GetThemeColor3ubv(TH_TAB_ACTIVE, theme_col_tab_active);
  UI_GetThemeColor3ubv(TH_TAB_INACTIVE, theme_col_tab_inactive);
  UI_GetThemeColor3ubv(TH_TAB_OUTLINE, theme_col_tab_outline);

  interp_v3_v3v3_uchar(theme_col_tab_divider, theme_col_back, theme_col_tab_outline, 0.3f);
  interp_v3_v3v3_uchar(theme_col_tab_highlight, theme_col_back, theme_col_text_hi, 0.2f);
  interp_v3_v3v3_uchar(
      theme_col_tab_highlight_inactive, theme_col_tab_inactive, theme_col_text_hi, 0.12f);

  is_alpha = (region->overlap && (theme_col_back[3] != 255));

  if (fstyle->kerning == 1) {
    BLF_enable(fstyle->uifont_id, BLF_KERNING_DEFAULT);
  }

  BLF_enable(fontid, BLF_ROTATION);
  BLF_rotation(fontid, M_PI_2);
  // UI_fontstyle_set(&style->widget);
  ui_fontscale(&fstyle_points, aspect / (U.pixelsize * 1.1f));
  BLF_size(fontid, fstyle_points, U.dpi);

  /* Check the region type supports categories to avoid an assert
   * for showing 3D view panels in the properties space. */
  if ((1 << region->regiontype) & RGN_TYPE_HAS_CATEGORY_MASK) {
    BLI_assert(UI_panel_category_is_visible(region));
  }

  /* Calculate tab rectangle and check if we need to scale down. */
  LISTBASE_FOREACH (PanelCategoryDyn *, pc_dyn, &region->panels_category) {

    rcti *rct = &pc_dyn->rect;
    const char *category_id = pc_dyn->idname;
    const char *category_id_draw = IFACE_(category_id);
    const int category_width = BLF_width(fontid, category_id_draw, BLF_DRAW_STR_DUMMY_MAX);

    rct->xmin = rct_xmin;
    rct->xmax = rct_xmax;

    rct->ymin = v2d->mask.ymax - (y_ofs + category_width + (tab_v_pad_text * 2));
    rct->ymax = v2d->mask.ymax - (y_ofs);

    y_ofs += category_width + tab_v_pad + (tab_v_pad_text * 2);
  }

  if (y_ofs > BLI_rcti_size_y(&v2d->mask)) {
    scaletabs = (float)BLI_rcti_size_y(&v2d->mask) / (float)y_ofs;

    LISTBASE_FOREACH (PanelCategoryDyn *, pc_dyn, &region->panels_category) {
      rcti *rct = &pc_dyn->rect;
      rct->ymin = ((rct->ymin - v2d->mask.ymax) * scaletabs) + v2d->mask.ymax;
      rct->ymax = ((rct->ymax - v2d->mask.ymax) * scaletabs) + v2d->mask.ymax;
    }

    do_scaletabs = true;
  }

  /* Begin drawing. */
  GPU_line_smooth(true);

  uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", GPU_COMP_I32, 2, GPU_FETCH_INT_TO_FLOAT);
  immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);

  /* Draw the background. */
  if (is_alpha) {
    GPU_blend(GPU_BLEND_ALPHA);
    immUniformColor4ubv(theme_col_tab_bg);
  }
  else {
    immUniformColor3ubv(theme_col_tab_bg);
  }

  if (is_left) {
    immRecti(
        pos, v2d->mask.xmin, v2d->mask.ymin, v2d->mask.xmin + category_tabs_width, v2d->mask.ymax);
  }
  else {
    immRecti(
        pos, v2d->mask.xmax - category_tabs_width, v2d->mask.ymin, v2d->mask.xmax, v2d->mask.ymax);
  }

  if (is_alpha) {
    GPU_blend(GPU_BLEND_NONE);
  }

  immUnbindProgram();

  const int divider_xmin = is_left ? (v2d->mask.xmin + (category_tabs_width - px)) :
                                     (v2d->mask.xmax - category_tabs_width) + px;
  const int divider_xmax = is_left ? (v2d->mask.xmin + category_tabs_width) :
                                     (v2d->mask.xmax - (category_tabs_width + px)) + px;

  LISTBASE_FOREACH (PanelCategoryDyn *, pc_dyn, &region->panels_category) {
    const rcti *rct = &pc_dyn->rect;
    const char *category_id = pc_dyn->idname;
    const char *category_id_draw = IFACE_(category_id);
    const int category_width = BLI_rcti_size_y(rct) - (tab_v_pad_text * 2);
    size_t category_draw_len = BLF_DRAW_STR_DUMMY_MAX;
#if 0
    int category_width = BLF_width(fontid, category_id_draw, BLF_DRAW_STR_DUMMY_MAX);
#endif

    const bool is_active = STREQ(category_id, category_id_active);

    GPU_blend(GPU_BLEND_ALPHA);

#ifdef USE_FLAT_INACTIVE
    if (is_active)
#endif
    {
      const bool use_flip_x = !is_left;
      ui_panel_category_draw_tab(true,
                                 rct->xmin,
                                 rct->ymin,
                                 rct->xmax,
                                 rct->ymax,
                                 tab_curve_radius - px,
                                 roundboxtype,
                                 true,
                                 true,
                                 use_flip_x,
                                 NULL,
                                 is_active ? theme_col_tab_active : theme_col_tab_inactive);

      /* Tab outline. */
      ui_panel_category_draw_tab(false,
                                 rct->xmin - px_x_sign,
                                 rct->ymin - px,
                                 rct->xmax - px_x_sign,
                                 rct->ymax + px,
                                 tab_curve_radius,
                                 roundboxtype,
                                 true,
                                 true,
                                 use_flip_x,
                                 NULL,
                                 theme_col_tab_outline);

      /* Tab highlight (3d look). */
      ui_panel_category_draw_tab(false,
                                 rct->xmin,
                                 rct->ymin,
                                 rct->xmax,
                                 rct->ymax,
                                 tab_curve_radius,
                                 roundboxtype,
                                 true,
                                 false,
                                 use_flip_x,
                                 is_active ? theme_col_back : theme_col_tab_inactive,
                                 is_active ? theme_col_tab_highlight :
                                             theme_col_tab_highlight_inactive);
    }

    /* Tab black-line. */
    if (!is_active) {
      pos = GPU_vertformat_attr_add(
          immVertexFormat(), "pos", GPU_COMP_I32, 2, GPU_FETCH_INT_TO_FLOAT);
      immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);

      immUniformColor3ubv(theme_col_tab_divider);
      immRecti(pos, divider_xmin, rct->ymin - tab_v_pad, divider_xmax, rct->ymax + tab_v_pad);
      immUnbindProgram();
    }

    if (do_scaletabs) {
      category_draw_len = BLF_width_to_strlen(
          fontid, category_id_draw, category_draw_len, category_width, NULL);
    }

    BLF_position(fontid, rct->xmax - text_v_ofs, rct->ymin + tab_v_pad_text, 0.0f);

    /* Tab titles. */

    /* Draw white shadow to give text more depth. */
    BLF_color3ubv(fontid, theme_col_text);

    /* Main tab title. */
    BLF_draw(fontid, category_id_draw, category_draw_len);

    GPU_blend(GPU_BLEND_NONE);

    /* Tab black-line remaining (last tab). */
    pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", GPU_COMP_I32, 2, GPU_FETCH_INT_TO_FLOAT);
    immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
    if (pc_dyn->prev == NULL) {
      immUniformColor3ubv(theme_col_tab_divider);
      immRecti(pos, divider_xmin, rct->ymax + px, divider_xmax, v2d->mask.ymax);
    }
    if (pc_dyn->next == NULL) {
      immUniformColor3ubv(theme_col_tab_divider);
      immRecti(pos, divider_xmin, 0, divider_xmax, rct->ymin);
    }

#ifdef USE_FLAT_INACTIVE
    /* Draw line between inactive tabs. */
    if (is_active == false && is_active_prev == false && pc_dyn->prev) {
      immUniformColor3ubv(theme_col_tab_divider);
      immRecti(pos,
               v2d->mask.xmin + (category_tabs_width / 5),
               rct->ymax + px,
               (v2d->mask.xmin + category_tabs_width) - (category_tabs_width / 5),
               rct->ymax + (px * 3));
    }

    is_active_prev = is_active;
#endif
    immUnbindProgram();

    /* Not essential, but allows events to be handled right up to the region edge (T38171). */
    if (is_left) {
      pc_dyn->rect.xmin = v2d->mask.xmin;
    }
    else {
      pc_dyn->rect.xmax = v2d->mask.xmax;
    }
  }

  GPU_line_smooth(false);

  BLF_disable(fontid, BLF_ROTATION);

  if (fstyle->kerning == 1) {
    BLF_disable(fstyle->uifont_id, BLF_KERNING_DEFAULT);
  }

#undef USE_FLAT_INACTIVE
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Panel Alignment
 * \{ */

static int get_panel_size_y(const Panel *panel)
{
  if (panel->type && (panel->type->flag & PNL_NO_HEADER)) {
    return panel->sizey;
  }

  return PNL_HEADER + panel->sizey;
}

static int get_panel_real_size_y(const Panel *panel)
{
  const int sizey = (panel->flag & PNL_CLOSED) ? 0 : panel->sizey;

  if (panel->type && (panel->type->flag & PNL_NO_HEADER)) {
    return sizey;
  }

  return PNL_HEADER + sizey;
}

int UI_panel_size_y(const Panel *panel)
{
  return get_panel_real_size_y(panel);
}

/**
 * This function is needed because #uiBlock and Panel itself don't
 * change #Panel.sizey or location when closed.
 */
static int get_panel_real_ofsy(Panel *panel)
{
  if (panel->flag & PNL_CLOSED) {
    return panel->ofsy + panel->sizey;
  }
  return panel->ofsy;
}

bool UI_panel_is_dragging(const struct Panel *panel)
{
  uiHandlePanelData *data = panel->activedata;
  if (!data) {
    return false;
  }

  return data->is_drag_drop;
}

/**
 * \note about sorting:
 * The #Panel.sortorder has a lower value for new panels being added.
 * however, that only works to insert a single panel, when more new panels get
 * added the coordinates of existing panels and the previously stored to-be-inserted
 * panels do not match for sorting.
 */

static int find_highest_panel(const void *a, const void *b)
{
  const Panel *panel_a = ((PanelSort *)a)->panel;
  const Panel *panel_b = ((PanelSort *)b)->panel;

  /* Stick uppermost header-less panels to the top of the region -
   * prevent them from being sorted (multiple header-less panels have to be sorted though). */
  if (panel_a->type->flag & PNL_NO_HEADER && panel_b->type->flag & PNL_NO_HEADER) {
    /* Skip and check for `ofsy` and #Panel.sortorder below. */
  }
  if (panel_a->type->flag & PNL_NO_HEADER) {
    return -1;
  }
  if (panel_b->type->flag & PNL_NO_HEADER) {
    return 1;
  }

  if (panel_a->ofsy + panel_a->sizey < panel_b->ofsy + panel_b->sizey) {
    return 1;
  }
  if (panel_a->ofsy + panel_a->sizey > panel_b->ofsy + panel_b->sizey) {
    return -1;
  }
  if (panel_a->sortorder > panel_b->sortorder) {
    return 1;
  }
  if (panel_a->sortorder < panel_b->sortorder) {
    return -1;
  }

  return 0;
}

static int compare_panel(const void *a, const void *b)
{
  const Panel *panel_a = ((PanelSort *)a)->panel;
  const Panel *panel_b = ((PanelSort *)b)->panel;

  if (panel_a->sortorder > panel_b->sortorder) {
    return 1;
  }
  if (panel_a->sortorder < panel_b->sortorder) {
    return -1;
  }

  return 0;
}

static void align_sub_panels(Panel *panel)
{
  /* Position sub panels. */
  int ofsy = panel->ofsy + panel->sizey - panel->blocksizey;

  LISTBASE_FOREACH (Panel *, pachild, &panel->children) {
    if (pachild->runtime_flag & PANEL_ACTIVE) {
      pachild->ofsx = panel->ofsx;
      pachild->ofsy = ofsy - get_panel_size_y(pachild);
      ofsy -= get_panel_real_size_y(pachild);

      if (pachild->children.first) {
        align_sub_panels(pachild);
      }
    }
  }
}

/**
 * Calculate the position and order of panels as they are opened, closed, and dragged.
 */
static bool uiAlignPanelStep(ARegion *region, const float factor, const bool drag)
{
  /* Count active panels. */
  int active_panels_len = 0;
  LISTBASE_FOREACH (Panel *, panel, &region->panels) {
    if (panel->runtime_flag & PANEL_ACTIVE) {
      /* These panels should have types since they are currently displayed to the user. */
      BLI_assert(panel->type != NULL);
      active_panels_len++;
    }
  }
  if (active_panels_len == 0) {
    return false;
  }

  /* Sort panels. */
  PanelSort *panel_sort = MEM_mallocN(sizeof(PanelSort) * active_panels_len, __func__);
  {
    PanelSort *ps = panel_sort;
    LISTBASE_FOREACH (Panel *, panel, &region->panels) {
      if (panel->runtime_flag & PANEL_ACTIVE) {
        ps->panel = panel;
        ps++;
      }
    }
  }

  if (drag) {
    /* While dragging, sort based on location and update #Panel.sortorder. */
    qsort(panel_sort, active_panels_len, sizeof(PanelSort), find_highest_panel);
    for (int i = 0; i < active_panels_len; i++) {
      panel_sort[i].panel->sortorder = i;
    }
  }
  else {
    /* Otherwise use #Panel.sortorder. */
    qsort(panel_sort, active_panels_len, sizeof(PanelSort), compare_panel);
  }

  /* X offset. */
  const int region_offset_x = panel_region_offset_x_get(region);
  for (int i = 0; i < active_panels_len; i++) {
    PanelSort *ps = &panel_sort[i];
    const bool use_box = ps->panel->type->flag & PNL_DRAW_BOX;
    ps->panel->runtime.region_ofsx = region_offset_x;
    ps->new_offset_x = region_offset_x + ((use_box) ? UI_PANEL_BOX_STYLE_MARGIN : 0);
  }

  /* Y offset. */
  for (int i = 0, y = 0; i < active_panels_len; i++) {
    PanelSort *ps = &panel_sort[i];
    y -= get_panel_real_size_y(ps->panel);

    const bool use_box = ps->panel->type->flag & PNL_DRAW_BOX;
    if (use_box) {
      y -= UI_PANEL_BOX_STYLE_MARGIN;
    }
    ps->new_offset_y = y;
    /* The header still draws offset by the size of closed panels, so apply the offset here. */
    if (ps->panel->flag & PNL_CLOSED) {
      panel_sort[i].new_offset_y -= ps->panel->sizey;
    }
  }

  /* Interpolate based on the input factor. */
  bool changed = false;
  for (int i = 0; i < active_panels_len; i++) {
    PanelSort *ps = &panel_sort[i];
    if (ps->panel->flag & PNL_SELECT) {
      continue;
    }

    if (ps->new_offset_x != ps->panel->ofsx) {
      const float x = interpf((float)ps->new_offset_x, (float)ps->panel->ofsx, factor);
      ps->panel->ofsx = round_fl_to_int(x);
      changed = true;
    }
    if (ps->new_offset_y != ps->panel->ofsy) {
      const float y = interpf((float)ps->new_offset_y, (float)ps->panel->ofsy, factor);
      ps->panel->ofsy = round_fl_to_int(y);
      changed = true;
    }
  }

  /* Set locations for tabbed and sub panels. */
  LISTBASE_FOREACH (Panel *, panel, &region->panels) {
    if (panel->runtime_flag & PANEL_ACTIVE) {
      if (panel->children.first) {
        align_sub_panels(panel);
      }
    }
  }

  MEM_freeN(panel_sort);

  return changed;
}

static void ui_panels_size(ARegion *region, int *r_x, int *r_y)
{
  int sizex = 0;
  int sizey = 0;

  /* Compute size taken up by panels, for setting in view2d. */
  LISTBASE_FOREACH (Panel *, panel, &region->panels) {
    if (panel->runtime_flag & PANEL_ACTIVE) {
      const int pa_sizex = panel->ofsx + panel->sizex;
      const int pa_sizey = get_panel_real_ofsy(panel);

      sizex = max_ii(sizex, pa_sizex);
      sizey = min_ii(sizey, pa_sizey);
    }
  }

  if (sizex == 0) {
    sizex = UI_PANEL_WIDTH;
  }
  if (sizey == 0) {
    sizey = -UI_PANEL_WIDTH;
  }

  *r_x = sizex;
  *r_y = sizey;
}

static void ui_do_animate(bContext *C, Panel *panel)
{
  uiHandlePanelData *data = panel->activedata;
  ARegion *region = CTX_wm_region(C);

  float fac = (PIL_check_seconds_timer() - data->starttime) / ANIMATION_TIME;
  fac = min_ff(sqrtf(fac), 1.0f);

  /* For max 1 second, interpolate positions. */
  if (uiAlignPanelStep(region, fac, false)) {
    ED_region_tag_redraw(region);
  }
  else {
    fac = 1.0f;
  }

  if (fac >= 1.0f) {
    /* Store before data is freed. */
    const bool is_drag_drop = data->is_drag_drop;

    panel_activate_state(C, panel, PANEL_STATE_EXIT);
    if (is_drag_drop) {
      /* Note: doing this in #panel_activate_state would require removing `const` for context in
       * many other places. */
      reorder_instanced_panel_list(C, region, panel);
    }
    return;
  }
}

static void panels_layout_begin_clear_flags(ListBase *lb)
{
  LISTBASE_FOREACH (Panel *, panel, lb) {
    /* Flags to copy over to the next layout pass. */
    const short flag_copy = 0;

    const bool was_active = panel->runtime_flag & PANEL_ACTIVE;
    panel->runtime_flag &= flag_copy;
    if (was_active) {
      panel->runtime_flag |= PANEL_WAS_ACTIVE;
    }

    panels_layout_begin_clear_flags(&panel->children);
  }
}

void UI_panels_begin(const bContext *UNUSED(C), ARegion *region)
{
  /* Set all panels as inactive, so that at the end we know which ones were used. Also
   * clear other flags so we know later that their values were set for the current redraw. */
  panels_layout_begin_clear_flags(&region->panels);
}

void UI_panels_end(const bContext *C, ARegion *region, int *r_x, int *r_y)
{
  ScrArea *area = CTX_wm_area(C);

  region_panels_set_expansion_from_list_data(C, region);

  /* Update panel expansion based on property search results. */
  if (region->flag & RGN_FLAG_SEARCH_FILTER_UPDATE) {
    /* Don't use the last update from the deactivation, or all the panels will be left closed. */
    if (region->flag & RGN_FLAG_SEARCH_FILTER_ACTIVE) {
      UI_panels_set_expansion_from_seach_filter(C, region);
      set_panels_list_data_expand_flag(C, region);
    }
  }

  /* Offset contents. */
  LISTBASE_FOREACH (uiBlock *, block, &region->uiblocks) {
    if (block->active && block->panel) {
      ui_offset_panel_block(block);
    }
  }

  /* Re-align, possibly with animation. */
  Panel *panel;
  if (panels_need_realign(area, region, &panel)) {
    if (panel) {
      panel_activate_state(C, panel, PANEL_STATE_ANIMATION);
    }
    else {
      uiAlignPanelStep(region, 1.0, false);
    }
  }

  /* Compute size taken up by panels. */
  ui_panels_size(region, r_x, r_y);
}

/**
 * Draw panels, selected (panels currently being dragged) on top.
 */
void UI_panels_draw(const bContext *C, ARegion *region)
{
  /* Draw in reverse order, because #uiBlocks are added in reverse order
   * and we need child panels to draw on top. */
  LISTBASE_FOREACH_BACKWARD (uiBlock *, block, &region->uiblocks) {
    if (block->active && block->panel && !(block->panel->flag & PNL_SELECT) &&
        !UI_block_is_search_only(block)) {
      UI_block_draw(C, block);
    }
  }

  LISTBASE_FOREACH_BACKWARD (uiBlock *, block, &region->uiblocks) {
    if (block->active && block->panel && (block->panel->flag & PNL_SELECT) &&
        !UI_block_is_search_only(block)) {
      UI_block_draw(C, block);
    }
  }
}

void UI_panels_scale(ARegion *region, float new_width)
{
  LISTBASE_FOREACH (uiBlock *, block, &region->uiblocks) {
    if (block->panel) {
      const float fac = new_width / (float)block->panel->sizex;
      block->panel->sizex = new_width;

      LISTBASE_FOREACH (uiBut *, but, &block->buttons) {
        but->rect.xmin *= fac;
        but->rect.xmax *= fac;
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Panel Dragging
 * \{ */

#define DRAG_REGION_PAD (PNL_HEADER * 0.5)
static void ui_do_drag(const bContext *C, const wmEvent *event, Panel *panel)
{
  uiHandlePanelData *data = panel->activedata;
  ARegion *region = CTX_wm_region(C);

  /* Keep the drag position in the region with a small pad to keep the panel visible. */
  const int x = clamp_i(event->x, region->winrct.xmin, region->winrct.xmax + DRAG_REGION_PAD);
  const int y = clamp_i(event->y, region->winrct.ymin, region->winrct.ymax + DRAG_REGION_PAD);

  float dx = (float)(x - data->startx);
  float dy = (float)(y - data->starty);

  /* Adjust for region zoom. */
  dx *= BLI_rctf_size_x(&region->v2d.cur) / (float)BLI_rcti_size_x(&region->winrct);
  dy *= BLI_rctf_size_y(&region->v2d.cur) / (float)BLI_rcti_size_y(&region->winrct);

  if (data->state == PANEL_STATE_DRAG_SCALE) {
    panel->sizex = MAX2(data->startsizex + dx, UI_PANEL_MINX);

    if (data->startsizey - dy < UI_PANEL_MINY) {
      dy = -UI_PANEL_MINY + data->startsizey;
    }

    panel->sizey = data->startsizey - dy;
    panel->ofsy = data->startofsy + dy;
  }
  else {
    /* Reset the panel snapping, to allow dragging away from snapped edges. */
    panel->snap = PNL_SNAP_NONE;

    /* Add the movement of the view due to edge scrolling while dragging. */
    dx += ((float)region->v2d.cur.xmin - data->start_cur_xmin);
    dy += ((float)region->v2d.cur.ymin - data->start_cur_ymin);
    panel->ofsx = data->startofsx + round_fl_to_int(dx);
    panel->ofsy = data->startofsy + round_fl_to_int(dy);

    uiAlignPanelStep(region, 0.2f, true);
  }

  ED_region_tag_redraw(region);
}
#undef DRAG_REGION_PAD

/** \} */

/* -------------------------------------------------------------------- */
/** \name Region Level Panel Interaction
 * \{ */

static uiPanelMouseState ui_panel_mouse_state_get(const uiBlock *block,
                                                  const Panel *panel,
                                                  const int mx,
                                                  const int my)
{
  if (!IN_RANGE((float)mx, block->rect.xmin, block->rect.xmax)) {
    return PANEL_MOUSE_OUTSIDE;
  }

  if (IN_RANGE((float)my, block->rect.ymax, block->rect.ymax + PNL_HEADER)) {
    return PANEL_MOUSE_INSIDE_HEADER;
  }

  if (!(panel->flag & PNL_CLOSED)) {
    if (IN_RANGE((float)my, block->rect.ymin, block->rect.ymax + PNL_HEADER)) {
      return PANEL_MOUSE_INSIDE_CONTENT;
    }
  }

  return PANEL_MOUSE_OUTSIDE;
}

typedef struct uiPanelDragCollapseHandle {
  bool was_first_open;
  int xy_init[2];
} uiPanelDragCollapseHandle;

static void ui_panel_drag_collapse_handler_remove(bContext *UNUSED(C), void *userdata)
{
  uiPanelDragCollapseHandle *dragcol_data = userdata;
  MEM_freeN(dragcol_data);
}

static void ui_panel_drag_collapse(const bContext *C,
                                   const uiPanelDragCollapseHandle *dragcol_data,
                                   const int xy_dst[2])
{
  ARegion *region = CTX_wm_region(C);

  LISTBASE_FOREACH (uiBlock *, block, &region->uiblocks) {
    float xy_a_block[2] = {UNPACK2(dragcol_data->xy_init)};
    float xy_b_block[2] = {UNPACK2(xy_dst)};
    Panel *panel = block->panel;

    if (panel == NULL || (panel->type && (panel->type->flag & PNL_NO_HEADER))) {
      continue;
    }
    const int oldflag = panel->flag;

    /* Lock axis. */
    xy_b_block[0] = dragcol_data->xy_init[0];

    /* Use cursor coords in block space. */
    ui_window_to_block_fl(region, block, &xy_a_block[0], &xy_a_block[1]);
    ui_window_to_block_fl(region, block, &xy_b_block[0], &xy_b_block[1]);

    /* Set up `rect` to match header size. */
    rctf rect = block->rect;
    rect.ymin = rect.ymax;
    rect.ymax = rect.ymin + PNL_HEADER;

    /* Touch all panels between last mouse coordinate and the current one. */
    if (BLI_rctf_isect_segment(&rect, xy_a_block, xy_b_block)) {
      /* Force panel to open or close. */
      SET_FLAG_FROM_TEST(panel->flag, dragcol_data->was_first_open, PNL_CLOSED);

      /* If panel->flag has changed this means a panel was opened/closed here. */
      if (panel->flag != oldflag) {
        panel_activate_state(C, panel, PANEL_STATE_ANIMATION);
      }
    }
  }
  /* Update the instanced panel data expand flags with the changes made here. */
  set_panels_list_data_expand_flag(C, region);
}

/**
 * Panel drag-collapse (modal handler).
 * Clicking and dragging over panels toggles their collapse state based on the panel
 * that was first dragged over. If it was open all affected panels including the initial
 * one are closed and vice versa.
 */
static int ui_panel_drag_collapse_handler(bContext *C, const wmEvent *event, void *userdata)
{
  wmWindow *win = CTX_wm_window(C);
  uiPanelDragCollapseHandle *dragcol_data = userdata;
  short retval = WM_UI_HANDLER_CONTINUE;

  switch (event->type) {
    case MOUSEMOVE:
      ui_panel_drag_collapse(C, dragcol_data, &event->x);

      retval = WM_UI_HANDLER_BREAK;
      break;
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        /* Done! */
        WM_event_remove_ui_handler(&win->modalhandlers,
                                   ui_panel_drag_collapse_handler,
                                   ui_panel_drag_collapse_handler_remove,
                                   dragcol_data,
                                   true);
        ui_panel_drag_collapse_handler_remove(C, dragcol_data);
      }
      /* Don't let any left-mouse event fall through! */
      retval = WM_UI_HANDLER_BREAK;
      break;
  }

  return retval;
}

static void ui_panel_drag_collapse_handler_add(const bContext *C, const bool was_open)
{
  wmWindow *win = CTX_wm_window(C);
  const wmEvent *event = win->eventstate;
  uiPanelDragCollapseHandle *dragcol_data = MEM_mallocN(sizeof(*dragcol_data), __func__);

  dragcol_data->was_first_open = was_open;
  copy_v2_v2_int(dragcol_data->xy_init, &event->x);

  WM_event_add_ui_handler(C,
                          &win->modalhandlers,
                          ui_panel_drag_collapse_handler,
                          ui_panel_drag_collapse_handler_remove,
                          dragcol_data,
                          0);
}

/**
 * Supposing the block has a panel and isn't a menu, handle opening, closing, pinning, etc.
 * Code currently assumes layout style for location of widgets
 *
 * \param mx: The mouse x coordinate, in panel space.
 */
static void ui_handle_panel_header(const bContext *C,
                                   uiBlock *block,
                                   const int mx,
                                   short int event_type,
                                   const short ctrl,
                                   const short shift)
{
  Panel *panel = block->panel;
  ARegion *region = CTX_wm_region(C);

  BLI_assert(panel->type != NULL);
  BLI_assert(!(panel->type->flag & PNL_NO_HEADER));

  const bool is_subpanel = (panel->type->parent != NULL);
  const bool use_pin = UI_panel_category_is_visible(region) && !is_subpanel;
  const bool show_pin = use_pin && (panel->flag & PNL_PIN);
  const bool show_drag = !is_subpanel;

  /* Handle panel pinning. */
  if (use_pin && ELEM(event_type, EVT_RETKEY, EVT_PADENTER, LEFTMOUSE) && shift) {
    panel->flag ^= PNL_PIN;
    ED_region_tag_redraw(region);
    return;
  }

  float expansion_area_xmax = block->rect.xmax;
  if (show_drag) {
    expansion_area_xmax -= (PNL_ICON * 1.5f);
  }
  if (show_pin) {
    expansion_area_xmax -= PNL_ICON;
  }

  /* Collapse and expand panels. */
  if (ELEM(event_type, EVT_RETKEY, EVT_PADENTER, EVT_AKEY) || mx < expansion_area_xmax) {
    if (ctrl && !is_subpanel) {
      /* For parent panels, collapse all other panels or toggle children. */
      if (panel->flag & PNL_CLOSED || BLI_listbase_is_empty(&panel->children)) {
        panels_collapse_all(region, panel);

        /* Reset the view - we don't want to display a view without content. */
        UI_view2d_offset(&region->v2d, 0.0f, 1.0f);
      }
      else {
        /* If a panel has sub-panels and it's open, toggle the expansion
         * of the sub-panels (based on the expansion of the first sub-panel). */
        Panel *first_child = panel->children.first;
        BLI_assert(first_child != NULL);
        panel_set_flag_recursive(panel, PNL_CLOSED, !(first_child->flag & PNL_CLOSED));
        panel->flag |= PNL_CLOSED;
      }
    }

    if (panel->flag & PNL_CLOSED) {
      panel->flag &= ~PNL_CLOSED;
      /* Snap back up so full panel aligns with screen edge. */
      if (panel->snap & PNL_SNAP_BOTTOM) {
        panel->ofsy = 0;
      }

      if (event_type == LEFTMOUSE) {
        ui_panel_drag_collapse_handler_add(C, false);
      }
    }
    else {
      /* Snap down to bottom screen edge. */
      panel->flag |= PNL_CLOSED;
      if (panel->snap & PNL_SNAP_BOTTOM) {
        panel->ofsy = -panel->sizey;
      }

      if (event_type == LEFTMOUSE) {
        ui_panel_drag_collapse_handler_add(C, true);
      }
    }

    set_panels_list_data_expand_flag(C, region);
    panel_activate_state(C, panel, PANEL_STATE_ANIMATION);
    return;
  }

  /* Handle panel dragging. For now don't allow dragging in floating regions. */
  if (show_drag && !(region->alignment == RGN_ALIGN_FLOAT)) {
    const float drag_area_xmin = block->rect.xmax - (PNL_ICON * 1.5f);
    const float drag_area_xmax = block->rect.xmax;
    if (IN_RANGE(mx, drag_area_xmin, drag_area_xmax)) {
      panel_activate_state(C, panel, PANEL_STATE_DRAG);
      return;
    }
  }

  /* Handle panel unpinning. */
  if (show_pin) {
    const float pin_area_xmin = expansion_area_xmax;
    const float pin_area_xmax = pin_area_xmin + PNL_ICON;
    if (IN_RANGE(mx, pin_area_xmin, pin_area_xmax)) {
      panel->flag ^= PNL_PIN;
      ED_region_tag_redraw(region);
      return;
    }
  }
}

bool UI_panel_category_is_visible(const ARegion *region)
{
  /* Check for more than one category. */
  return region->panels_category.first &&
         region->panels_category.first != region->panels_category.last;
}

PanelCategoryDyn *UI_panel_category_find(ARegion *region, const char *idname)
{
  return BLI_findstring(&region->panels_category, idname, offsetof(PanelCategoryDyn, idname));
}

PanelCategoryStack *UI_panel_category_active_find(ARegion *region, const char *idname)
{
  return BLI_findstring(
      &region->panels_category_active, idname, offsetof(PanelCategoryStack, idname));
}

static void ui_panel_category_active_set(ARegion *region, const char *idname, bool fallback)
{
  ListBase *lb = &region->panels_category_active;
  PanelCategoryStack *pc_act = UI_panel_category_active_find(region, idname);

  if (pc_act) {
    BLI_remlink(lb, pc_act);
  }
  else {
    pc_act = MEM_callocN(sizeof(PanelCategoryStack), __func__);
    BLI_strncpy(pc_act->idname, idname, sizeof(pc_act->idname));
  }

  if (fallback) {
    /* For fall-backs, add at the end so explicitly chosen categories have priority. */
    BLI_addtail(lb, pc_act);
  }
  else {
    BLI_addhead(lb, pc_act);
  }

  /* Validate all active panels. We could do this on load, they are harmless -
   * but we should remove them somewhere.
   * (Add-ons could define panels and gather cruft over time). */
  {
    PanelCategoryStack *pc_act_next;
    /* intentionally skip first */
    pc_act_next = pc_act->next;
    while ((pc_act = pc_act_next)) {
      pc_act_next = pc_act->next;
      if (!BLI_findstring(
              &region->type->paneltypes, pc_act->idname, offsetof(PanelType, category))) {
        BLI_remlink(lb, pc_act);
        MEM_freeN(pc_act);
      }
    }
  }
}

void UI_panel_category_active_set(ARegion *region, const char *idname)
{
  ui_panel_category_active_set(region, idname, false);
}

void UI_panel_category_active_set_default(ARegion *region, const char *idname)
{
  if (!UI_panel_category_active_find(region, idname)) {
    ui_panel_category_active_set(region, idname, true);
  }
}

const char *UI_panel_category_active_get(ARegion *region, bool set_fallback)
{
  LISTBASE_FOREACH (PanelCategoryStack *, pc_act, &region->panels_category_active) {
    if (UI_panel_category_find(region, pc_act->idname)) {
      return pc_act->idname;
    }
  }

  if (set_fallback) {
    PanelCategoryDyn *pc_dyn = region->panels_category.first;
    if (pc_dyn) {
      ui_panel_category_active_set(region, pc_dyn->idname, true);
      return pc_dyn->idname;
    }
  }

  return NULL;
}

PanelCategoryDyn *UI_panel_category_find_mouse_over_ex(ARegion *region, const int x, const int y)
{
  LISTBASE_FOREACH (PanelCategoryDyn *, ptd, &region->panels_category) {
    if (BLI_rcti_isect_pt(&ptd->rect, x, y)) {
      return ptd;
    }
  }

  return NULL;
}

PanelCategoryDyn *UI_panel_category_find_mouse_over(ARegion *region, const wmEvent *event)
{
  return UI_panel_category_find_mouse_over_ex(region, event->mval[0], event->mval[1]);
}

void UI_panel_category_add(ARegion *region, const char *name)
{
  PanelCategoryDyn *pc_dyn = MEM_callocN(sizeof(*pc_dyn), __func__);
  BLI_addtail(&region->panels_category, pc_dyn);

  BLI_strncpy(pc_dyn->idname, name, sizeof(pc_dyn->idname));

  /* 'pc_dyn->rect' must be set on draw. */
}

void UI_panel_category_clear_all(ARegion *region)
{
  BLI_freelistN(&region->panels_category);
}

static int ui_handle_panel_category_cycling(const wmEvent *event,
                                            ARegion *region,
                                            const uiBut *active_but)
{
  const bool is_mousewheel = ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE);
  const bool inside_tabregion =
      ((RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT) ?
           (event->mval[0] < ((PanelCategoryDyn *)region->panels_category.first)->rect.xmax) :
           (event->mval[0] > ((PanelCategoryDyn *)region->panels_category.first)->rect.xmin));

  /* If mouse is inside non-tab region, ctrl key is required. */
  if (is_mousewheel && !event->ctrl && !inside_tabregion) {
    return WM_UI_HANDLER_CONTINUE;
  }

  if (active_but && ui_but_supports_cycling(active_but)) {
    /* Skip - exception to make cycling buttons using ctrl+mousewheel work in tabbed regions. */
  }
  else {
    const char *category = UI_panel_category_active_get(region, false);
    if (LIKELY(category)) {
      PanelCategoryDyn *pc_dyn = UI_panel_category_find(region, category);
      if (LIKELY(pc_dyn)) {
        if (is_mousewheel) {
          /* We can probably get rid of this and only allow ctrl-tabbing. */
          pc_dyn = (event->type == WHEELDOWNMOUSE) ? pc_dyn->next : pc_dyn->prev;
        }
        else {
          const bool backwards = event->shift;
          pc_dyn = backwards ? pc_dyn->prev : pc_dyn->next;
          if (!pc_dyn) {
            /* Proper cyclic behavior, back to first/last category (only used for ctrl+tab). */
            pc_dyn = backwards ? region->panels_category.last : region->panels_category.first;
          }
        }

        if (pc_dyn) {
          /* Intentionally don't reset scroll in this case,
           * allowing for quick browsing between tabs. */
          UI_panel_category_active_set(region, pc_dyn->idname);
          ED_region_tag_redraw(region);
        }
      }
    }
    return WM_UI_HANDLER_BREAK;
  }

  return WM_UI_HANDLER_CONTINUE;
}

/**
 * Handle region panel events like opening and closing panels, changing categories, etc.
 *
 * \note Could become a modal key-map.
 */
int ui_handler_panel_region(bContext *C,
                            const wmEvent *event,
                            ARegion *region,
                            const uiBut *active_but)
{
  /* Mouse-move events are handled by separate handlers for dragging and drag collapsing. */
  if (ELEM(event->type, MOUSEMOVE, INBETWEEN_MOUSEMOVE)) {
    return WM_UI_HANDLER_CONTINUE;
  }

  /* We only use KM_PRESS events in this function, so it's simpler to return early. */
  if (event->val != KM_PRESS) {
    return WM_UI_HANDLER_CONTINUE;
  }

  /* Scroll-bars can overlap panels now, they have handling priority. */
  if (UI_view2d_mouse_in_scrollers(region, &region->v2d, event->x, event->y)) {
    return WM_UI_HANDLER_CONTINUE;
  }

  int retval = WM_UI_HANDLER_CONTINUE;

  /* Handle category tabs. */
  if (UI_panel_category_is_visible(region)) {
    if (event->type == LEFTMOUSE) {
      PanelCategoryDyn *pc_dyn = UI_panel_category_find_mouse_over(region, event);
      if (pc_dyn) {
        UI_panel_category_active_set(region, pc_dyn->idname);
        ED_region_tag_redraw(region);

        /* Reset scroll to the top (T38348). */
        UI_view2d_offset(&region->v2d, -1.0f, 1.0f);

        retval = WM_UI_HANDLER_BREAK;
      }
    }
    else if ((event->type == EVT_TABKEY && event->ctrl) ||
             ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE)) {
      /* Cycle tabs. */
      retval = ui_handle_panel_category_cycling(event, region, active_but);
    }
  }

  if (retval == WM_UI_HANDLER_BREAK) {
    return retval;
  }

  const bool region_has_active_button = (ui_region_find_active_but(region) != NULL);

  LISTBASE_FOREACH (uiBlock *, block, &region->uiblocks) {
    Panel *panel = block->panel;
    if (panel == NULL || panel->type == NULL) {
      continue;
    }
    /* We can't expand or collapse panels without headers, they would disappear. */
    if (panel->type->flag & PNL_NO_HEADER) {
      continue;
    }

    int mx = event->x;
    int my = event->y;
    ui_window_to_block(region, block, &mx, &my);

    const uiPanelMouseState mouse_state = ui_panel_mouse_state_get(block, panel, mx, my);

    /* The panel collapse / expand key "A" is special as it takes priority over
     * active button handling. */
    if (ELEM(mouse_state, PANEL_MOUSE_INSIDE_CONTENT, PANEL_MOUSE_INSIDE_HEADER)) {
      if (event->type == EVT_AKEY && !IS_EVENT_MOD(event, shift, ctrl, alt, oskey)) {
        retval = WM_UI_HANDLER_BREAK;
        ui_handle_panel_header(C, block, mx, event->type, event->ctrl, event->shift);
        break;
      }
    }

    /* Don't do any other panel handling with an active button. */
    if (region_has_active_button) {
      continue;
    }

    /* All mouse clicks inside panels should return in break, but continue handling
     * in case there is a sub-panel header at the mouse location. */
    if (event->type == LEFTMOUSE &&
        ELEM(mouse_state, PANEL_MOUSE_INSIDE_CONTENT, PANEL_MOUSE_INSIDE_HEADER)) {
      retval = WM_UI_HANDLER_BREAK;
    }

    if (mouse_state == PANEL_MOUSE_INSIDE_HEADER) {
      if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER, LEFTMOUSE)) {
        retval = WM_UI_HANDLER_BREAK;
        ui_handle_panel_header(C, block, mx, event->type, event->ctrl, event->shift);
      }
      else if (event->type == RIGHTMOUSE) {
        retval = WM_UI_HANDLER_BREAK;
        ui_popup_context_menu_for_panel(C, region, block->panel);
      }
      break;
    }
  }

  return retval;
}

static void ui_panel_custom_data_set_recursive(Panel *panel, PointerRNA *custom_data)
{
  panel->runtime.custom_data_ptr = custom_data;

  LISTBASE_FOREACH (Panel *, child_panel, &panel->children) {
    ui_panel_custom_data_set_recursive(child_panel, custom_data);
  }
}

void UI_panel_custom_data_set(Panel *panel, PointerRNA *custom_data)
{
  BLI_assert(panel->type != NULL);

  /* Free the old custom data, which should be shared among all of the panel's sub-panels. */
  if (panel->runtime.custom_data_ptr != NULL) {
    MEM_freeN(panel->runtime.custom_data_ptr);
  }

  ui_panel_custom_data_set_recursive(panel, custom_data);
}

PointerRNA *UI_panel_custom_data_get(const Panel *panel)
{
  return panel->runtime.custom_data_ptr;
}

PointerRNA *UI_region_panel_custom_data_under_cursor(const bContext *C, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);

  Panel *panel = NULL;
  LISTBASE_FOREACH (uiBlock *, block, &region->uiblocks) {
    panel = block->panel;
    if (panel == NULL) {
      continue;
    }

    int mx = event->x;
    int my = event->y;
    ui_window_to_block(region, block, &mx, &my);
    const int mouse_state = ui_panel_mouse_state_get(block, panel, mx, my);
    if (ELEM(mouse_state, PANEL_MOUSE_INSIDE_CONTENT, PANEL_MOUSE_INSIDE_HEADER)) {
      break;
    }
  }

  if (panel == NULL) {
    return NULL;
  }

  return UI_panel_custom_data_get(panel);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window Level Modal Panel Interaction
 * \{ */

/* Note, this is modal handler and should not swallow events for animation. */
static int ui_handler_panel(bContext *C, const wmEvent *event, void *userdata)
{
  Panel *panel = userdata;
  uiHandlePanelData *data = panel->activedata;

  /* Verify if we can stop. */
  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    panel_activate_state(C, panel, PANEL_STATE_ANIMATION);
  }
  else if (event->type == MOUSEMOVE) {
    if (data->state == PANEL_STATE_DRAG) {
      ui_do_drag(C, event, panel);
    }
  }
  else if (event->type == TIMER && event->customdata == data->animtimer) {
    if (data->state == PANEL_STATE_ANIMATION) {
      ui_do_animate(C, panel);
    }
    else if (data->state == PANEL_STATE_DRAG) {
      ui_do_drag(C, event, panel);
    }
  }

  data = panel->activedata;

  if (data && data->state == PANEL_STATE_ANIMATION) {
    return WM_UI_HANDLER_CONTINUE;
  }
  return WM_UI_HANDLER_BREAK;
}

static void ui_handler_remove_panel(bContext *C, void *userdata)
{
  Panel *panel = userdata;

  panel_activate_state(C, panel, PANEL_STATE_EXIT);
}

static void panel_activate_state(const bContext *C, Panel *panel, uiHandlePanelState state)
{
  uiHandlePanelData *data = panel->activedata;
  wmWindow *win = CTX_wm_window(C);
  ARegion *region = CTX_wm_region(C);

  if (data && data->state == state) {
    return;
  }

  const bool was_drag_drop = (data && data->state == PANEL_STATE_DRAG);

  /* Set selection state for the panel and its sub-panels, which need to know they are selected
   * too so they can be drawn above their parent when it's dragged. */
  if (state == PANEL_STATE_EXIT || state == PANEL_STATE_ANIMATION) {
    panel_set_flag_recursive(panel, PNL_SELECT, false);
  }
  else {
    panel_set_flag_recursive(panel, PNL_SELECT, true);
  }

  if (data && data->animtimer) {
    WM_event_remove_timer(CTX_wm_manager(C), win, data->animtimer);
    data->animtimer = NULL;
  }

  if (state == PANEL_STATE_EXIT) {
    MEM_freeN(data);
    panel->activedata = NULL;

    WM_event_remove_ui_handler(
        &win->modalhandlers, ui_handler_panel, ui_handler_remove_panel, panel, false);
  }
  else {
    if (!data) {
      data = MEM_callocN(sizeof(uiHandlePanelData), "uiHandlePanelData");
      panel->activedata = data;

      WM_event_add_ui_handler(
          C, &win->modalhandlers, ui_handler_panel, ui_handler_remove_panel, panel, 0);
    }

    if (ELEM(state, PANEL_STATE_ANIMATION, PANEL_STATE_DRAG)) {
      data->animtimer = WM_event_add_timer(CTX_wm_manager(C), win, TIMER, ANIMATION_INTERVAL);
    }

    /* Initiate edge panning during drags so we can move beyond the initial region view. */
    if (state == PANEL_STATE_DRAG) {
      wmOperatorType *ot = WM_operatortype_find("VIEW2D_OT_edge_pan", true);
      ui_handle_afterfunc_add_operator(ot, WM_OP_INVOKE_DEFAULT, true);
    }

    data->state = state;
    data->startx = win->eventstate->x;
    data->starty = win->eventstate->y;
    data->startofsx = panel->ofsx;
    data->startofsy = panel->ofsy;
    data->startsizex = panel->sizex;
    data->startsizey = panel->sizey;
    data->start_cur_xmin = region->v2d.cur.xmin;
    data->start_cur_ymin = region->v2d.cur.ymin;
    data->starttime = PIL_check_seconds_timer();

    /* Remember drag drop state even when animating to the aligned position after dragging. */
    data->is_drag_drop = was_drag_drop;
    if (state == PANEL_STATE_DRAG) {
      data->is_drag_drop = true;
    }
  }

  ED_region_tag_redraw(region);
}

PanelType *UI_paneltype_find(int space_id, int region_id, const char *idname)
{
  SpaceType *st = BKE_spacetype_from_id(space_id);
  if (st) {
    ARegionType *art = BKE_regiontype_from_id(st, region_id);
    if (art) {
      return BLI_findstring(&art->paneltypes, idname, offsetof(PanelType, idname));
    }
  }
  return NULL;
}

/** \} */
