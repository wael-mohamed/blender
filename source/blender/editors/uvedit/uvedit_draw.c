/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
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
 *
 * Contributor(s): Blender Foundation, 2002-2009
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/uvedit/uvedit_draw.c
 *  \ingroup eduv
 */


#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "../../draw/intern/draw_cache_impl.h"

#include "BLI_math.h"
#include "BLI_utildefines.h"
#include "BLI_buffer.h"
#include "BLI_bitmap.h"

#include "BKE_deform.h"
#include "BKE_editmesh.h"
#include "BKE_material.h"
#include "BKE_layer.h"

#include "BKE_scene.h"

#include "BIF_glutil.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "GPU_batch.h"
#include "GPU_immediate.h"
#include "GPU_immediate_util.h"
#include "GPU_matrix.h"
#include "GPU_state.h"
#include "GPU_draw.h"

#include "ED_image.h"
#include "ED_mesh.h"
#include "ED_uvedit.h"

#include "UI_resources.h"
#include "UI_interface.h"
#include "UI_view2d.h"

#include "uvedit_intern.h"

static void draw_uvs_lineloop_bmfaces(BMesh *bm, const int cd_loop_uv_offset, const uint shdr_pos);

void ED_image_draw_cursor(ARegion *ar, const float cursor[2])
{
	float zoom[2], x_fac, y_fac;

	UI_view2d_scale_get_inverse(&ar->v2d, &zoom[0], &zoom[1]);

	mul_v2_fl(zoom, 256.0f * UI_DPI_FAC);
	x_fac = zoom[0];
	y_fac = zoom[1];

	GPU_line_width(1.0f);

	GPU_matrix_translate_2fv(cursor);

	const uint shdr_pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

	immBindBuiltinProgram(GPU_SHADER_2D_LINE_DASHED_UNIFORM_COLOR);

	float viewport_size[4];
	GPU_viewport_size_get_f(viewport_size);
	immUniform2f("viewport_size", viewport_size[2] / UI_DPI_FAC, viewport_size[3] / UI_DPI_FAC);

	immUniform1i("colors_len", 2);  /* "advanced" mode */
	immUniformArray4fv("colors", (float *)(float[][4]){{1.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}}, 2);
	immUniform1f("dash_width", 8.0f);

	immBegin(GPU_PRIM_LINES, 8);

	immVertex2f(shdr_pos, -0.05f * x_fac, 0.0f);
	immVertex2f(shdr_pos, 0.0f, 0.05f * y_fac);

	immVertex2f(shdr_pos, 0.0f, 0.05f * y_fac);
	immVertex2f(shdr_pos, 0.05f * x_fac, 0.0f);

	immVertex2f(shdr_pos, 0.05f * x_fac, 0.0f);
	immVertex2f(shdr_pos, 0.0f, -0.05f * y_fac);

	immVertex2f(shdr_pos, 0.0f, -0.05f * y_fac);
	immVertex2f(shdr_pos, -0.05f * x_fac, 0.0f);

	immEnd();

	immUniformArray4fv("colors", (float *)(float[][4]){{1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}}, 2);
	immUniform1f("dash_width", 2.0f);

	immBegin(GPU_PRIM_LINES, 8);

	immVertex2f(shdr_pos, -0.020f * x_fac, 0.0f);
	immVertex2f(shdr_pos, -0.1f * x_fac, 0.0f);

	immVertex2f(shdr_pos, 0.1f * x_fac, 0.0f);
	immVertex2f(shdr_pos, 0.020f * x_fac, 0.0f);

	immVertex2f(shdr_pos, 0.0f, -0.020f * y_fac);
	immVertex2f(shdr_pos, 0.0f, -0.1f * y_fac);

	immVertex2f(shdr_pos, 0.0f, 0.1f * y_fac);
	immVertex2f(shdr_pos, 0.0f, 0.020f * y_fac);

	immEnd();

	immUnbindProgram();

	GPU_matrix_translate_2f(-cursor[0], -cursor[1]);
}

static int draw_uvs_face_check(Scene *scene)
{
	ToolSettings *ts = scene->toolsettings;

	/* checks if we are selecting only faces */
	if (ts->uv_flag & UV_SYNC_SELECTION) {
		if (ts->selectmode == SCE_SELECT_FACE)
			return 2;
		else if (ts->selectmode & SCE_SELECT_FACE)
			return 1;
		else
			return 0;
	}
	else
		return (ts->uv_selectmode == UV_SELECT_FACE);
}

static void draw_uvs_shadow(Object *obedit)
{
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	BMesh *bm = em->bm;

	if (bm->totloop == 0) {
		return;
	}

	const int cd_loop_uv_offset = CustomData_get_offset(&bm->ldata, CD_MLOOPUV);

	uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

	immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);

	/* draws the mesh when painting */
	immUniformThemeColor(TH_UV_SHADOW);

	draw_uvs_lineloop_bmfaces(bm, cd_loop_uv_offset, pos);

	immUnbindProgram();
}

static void draw_uvs_lineloop_bmfaces(BMesh *bm, const int cd_loop_uv_offset, const uint shdr_pos)
{
	BMIter iter, liter;
	BMFace *efa;
	BMLoop *l;
	MLoopUV *luv;

	/* For more efficiency first transfer the entire buffer to vram. */
	GPUBatch *loop_batch = immBeginBatchAtMost(GPU_PRIM_LINE_LOOP, bm->totloop);

	BM_ITER_MESH(efa, &iter, bm, BM_FACES_OF_MESH) {
		if (!BM_elem_flag_test(efa, BM_ELEM_TAG))
			continue;

		BM_ITER_ELEM(l, &liter, efa, BM_LOOPS_OF_FACE) {
			luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
			immVertex2fv(shdr_pos, luv->uv);
		}
	}
	immEnd();

	/* Then draw each face contour separately. */
	GPU_batch_program_use_begin(loop_batch);
	unsigned int index = 0;
	BM_ITER_MESH(efa, &iter, bm, BM_FACES_OF_MESH) {
		if (!BM_elem_flag_test(efa, BM_ELEM_TAG))
			continue;

		GPU_batch_draw_range_ex(loop_batch, index, efa->len, false);
		index += efa->len;
	}
	GPU_batch_program_use_end(loop_batch);
	GPU_batch_discard(loop_batch);
}

static void draw_uvs_texpaint(Scene *scene, Object *ob)
{
	Mesh *me = ob->data;
	Material *ma;

	ma = give_current_material(ob, ob->actcol);

	if (me->mloopuv) {
		MPoly *mpoly = me->mpoly;
		MLoopUV *mloopuv, *mloopuv_base;
		int a, b;
		if (!(ma && ma->texpaintslot && ma->texpaintslot[ma->paint_active_slot].uvname &&
		      (mloopuv = CustomData_get_layer_named(&me->ldata, CD_MLOOPUV, ma->texpaintslot[ma->paint_active_slot].uvname))))
		{
			mloopuv = me->mloopuv;
		}

		uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

		immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);

		immUniformThemeColor(TH_UV_SHADOW);

		mloopuv_base = mloopuv;

		for (a = me->totpoly; a > 0; a--, mpoly++) {
			if ((scene->toolsettings->uv_flag & UV_SHOW_SAME_IMAGE) && mpoly->mat_nr != ob->actcol - 1)
				continue;

			immBegin(GPU_PRIM_LINE_LOOP, mpoly->totloop);

			mloopuv = mloopuv_base + mpoly->loopstart;
			for (b = 0; b < mpoly->totloop; b++, mloopuv++) {
				immVertex2fv(pos, mloopuv->uv);
			}

			immEnd();
		}

		immUnbindProgram();
	}
}

static uchar get_state(SpaceImage *sima, Scene *scene)
{
	ToolSettings *ts = scene->toolsettings;
	int drawfaces = draw_uvs_face_check(scene);
	const bool draw_stretch = (sima->flag & SI_DRAW_STRETCH) != 0;
	uchar state = UVEDIT_EDGES | UVEDIT_DATA;

	if (drawfaces) {
		state |= UVEDIT_FACEDOTS;
	}
	if (draw_stretch || !(sima->flag & SI_NO_DRAWFACES)) {
		state |= UVEDIT_FACES;

		if (draw_stretch) {
			if (sima->dt_uvstretch == SI_UVDT_STRETCH_AREA) {
				state |= UVEDIT_STRETCH_AREA;
			}
			else {
				state |= UVEDIT_STRETCH_ANGLE;
			}
		}
	}
	if (ts->uv_flag & UV_SYNC_SELECTION) {
		state |= UVEDIT_SYNC_SEL;
	}
	return state;
}

/* draws uv's in the image space */
static void draw_uvs(SpaceImage *sima, Scene *scene, Object *obedit, Depsgraph *depsgraph)
{
	GPUBatch *faces, *edges, *verts, *facedots;
	Object *eval_ob = DEG_get_evaluated_object(depsgraph, obedit);
	ToolSettings *ts = scene->toolsettings;
	float col1[4], col2[4], col3[4], transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	if (sima->flag & SI_DRAWSHADOW) {
		/* XXX TODO: Need to check if shadow mesh is different than original mesh. */
		bool is_cage_like_final_meshes = true;

		/* When sync selection is enabled, all faces are drawn (except for hidden)
		 * so if cage is the same as the final, there is no point in drawing this. */
		if (!((ts->uv_flag & UV_SYNC_SELECTION) && is_cage_like_final_meshes)) {
			draw_uvs_shadow(eval_ob);
		}
	}

	uchar state = get_state(sima, scene);

	DRW_mesh_cache_uvedit(
	        eval_ob, sima, scene, state,
	        &faces, &edges, &verts, &facedots);

	bool interpedges;
	bool do_elem_order_fix = (ts->uv_flag & UV_SYNC_SELECTION) && (ts->selectmode & SCE_SELECT_FACE);
	bool do_selected_edges = ((sima->flag & SI_NO_DRAWEDGES) == 0);
	bool draw_stretch = (state & (UVEDIT_STRETCH_AREA | UVEDIT_STRETCH_ANGLE)) != 0;
	if (ts->uv_flag & UV_SYNC_SELECTION) {
		interpedges = (ts->selectmode & SCE_SELECT_VERTEX) != 0;
	}
	else {
		interpedges = (ts->uv_selectmode == UV_SELECT_VERTEX);
	}

	GPU_blend_set_func_separate(GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);

	if (faces) {
		GPU_batch_program_set_builtin(faces, (draw_stretch)
		                                     ? GPU_SHADER_2D_UV_FACES_STRETCH
		                                     : GPU_SHADER_2D_UV_FACES);

		if (!draw_stretch) {
			GPU_blend(true);

			UI_GetThemeColor4fv(TH_FACE, col1);
			UI_GetThemeColor4fv(TH_FACE_SELECT, col2);
			UI_GetThemeColor4fv(TH_EDITMESH_ACTIVE, col3);
			col3[3] *= 0.2; /* Simulate dithering */
			GPU_batch_uniform_4fv(faces, "faceColor", col1);
			GPU_batch_uniform_4fv(faces, "selectColor", col2);
			GPU_batch_uniform_4fv(faces, "activeColor", col3);
		}

		GPU_batch_draw(faces);

		if (!draw_stretch) {
			GPU_blend(false);
		}
	}
	if (edges) {
		if (sima->flag & SI_SMOOTH_UV) {
			GPU_line_smooth(true);
			GPU_blend(true);
		}
		switch (sima->dt_uv) {
			case SI_UVDT_DASH:
			{
				float dash_colors[2][4] = {{0.56f, 0.56f, 0.56f, 1.0f}, {0.07f, 0.07f, 0.07f, 1.0f}};
				float viewport_size[4];
				GPU_viewport_size_get_f(viewport_size);

				GPU_line_width(1.0f);
				GPU_batch_program_set_builtin(edges, GPU_SHADER_2D_LINE_DASHED_UNIFORM_COLOR);
				GPU_batch_uniform_4fv_array(edges, "colors", 2, (float *)dash_colors);
				GPU_batch_uniform_2f(edges, "viewport_size", viewport_size[2] / UI_DPI_FAC, viewport_size[3] / UI_DPI_FAC);
				GPU_batch_uniform_1i(edges, "colors_len", 2);  /* "advanced" mode */
				GPU_batch_uniform_1f(edges, "dash_width", 4.0f);
				GPU_batch_draw(edges);
				break;
			}
			case SI_UVDT_BLACK:
			case SI_UVDT_WHITE:
			{
				GPU_line_width(1.0f);
				GPU_batch_program_set_builtin(edges, GPU_SHADER_2D_UNIFORM_COLOR);
				if (sima->dt_uv == SI_UVDT_WHITE) {
					GPU_batch_uniform_4f(edges, "color", 1.0f, 1.0f, 1.0f, 1.0f);
				}
				else {
					GPU_batch_uniform_4f(edges, "color", 0.0f, 0.0f, 0.0f, 1.0f);
				}
				GPU_batch_draw(edges);
				break;
			}
			case SI_UVDT_OUTLINE:
			{
				GPU_line_width(3.0f);
				GPU_batch_program_set_builtin(edges, GPU_SHADER_2D_UNIFORM_COLOR);
				GPU_batch_uniform_4f(edges, "color", 0.0f, 0.0f, 0.0f, 1.0f);
				GPU_batch_draw(edges);

				UI_GetThemeColor4fv(TH_WIRE_EDIT, col1);
				UI_GetThemeColor4fv(TH_EDGE_SELECT, col2);

				/* We could modify the vbo's data filling instead of modifying the provoking vert. */
				glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);

				GPU_line_width(1.0f);
				GPU_batch_program_set_builtin(edges, (interpedges)
				                                     ? GPU_SHADER_2D_UV_EDGES_SMOOTH
				                                     : GPU_SHADER_2D_UV_EDGES);
				GPU_batch_uniform_4fv(edges, "edgeColor", col1);
				GPU_batch_uniform_4fv(edges, "selectColor", do_selected_edges ? col2 : col1);
				GPU_batch_draw(edges);

				if (do_elem_order_fix && do_selected_edges) {
					/* We have problem in this mode when face order make some edges
					 * appear unselected because an adjacent face is not selected and
					 * render after the selected face.
					 * So, to avoid sorting edges by state we just render selected edges
					 * on top. A bit overkill but it's simple. */
					GPU_blend(true);
					GPU_batch_uniform_4fv(edges, "edgeColor", transparent);
					GPU_batch_uniform_4fv(edges, "selectColor", col2);
					GPU_batch_draw(edges);
					GPU_blend(false);
				}
				glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
				break;
			}
		}
		if (sima->flag & SI_SMOOTH_UV) {
			GPU_line_smooth(false);
			GPU_blend(false);
		}
	}
	if (verts || facedots) {
		float pointsize = UI_GetThemeValuef(TH_VERTEX_SIZE);
		UI_GetThemeColor4fv(TH_VERTEX_SELECT, col2);
		if (verts) {
			float pinned_col[4] = {1.0f, 0.0f, 0.0f, 1.0f}; /* TODO Theme? */
			UI_GetThemeColor4fv(TH_VERTEX, col1);
			GPU_blend(true);
			GPU_enable_program_point_size();

			GPU_batch_program_set_builtin(verts, GPU_SHADER_2D_UV_VERTS);
			GPU_batch_uniform_4f(verts, "vertColor", col1[0], col1[1], col1[2], 1.0f);
			GPU_batch_uniform_4fv(verts, "selectColor", (do_elem_order_fix) ? transparent : col2);
			GPU_batch_uniform_4fv(verts, "pinnedColor", pinned_col);
			GPU_batch_uniform_1f(verts, "pointSize", (pointsize + 1.5f) * M_SQRT2);
			GPU_batch_uniform_1f(verts, "outlineWidth", 0.75f);
			GPU_batch_draw(verts);

			if (do_elem_order_fix) {
				/* We have problem in this mode when face order make some verts
				 * appear unselected because an adjacent face is not selected and
				 * render after the selected face.
				 * So, to avoid sorting verts by state we just render selected verts
				 * on top. A bit overkill but it's simple. */
				GPU_batch_uniform_4fv(verts, "vertColor", transparent);
				GPU_batch_uniform_4fv(verts, "selectColor", col2);
				GPU_batch_uniform_4fv(verts, "pinnedColor", pinned_col);
				GPU_batch_uniform_1f(verts, "pointSize", (pointsize + 1.5f) * M_SQRT2);
				GPU_batch_uniform_1f(verts, "outlineWidth", 0.75f);
				GPU_batch_draw(verts);
			}

			GPU_blend(false);
			GPU_disable_program_point_size();
		}
		if (facedots) {
			GPU_point_size(pointsize);

			UI_GetThemeColor4fv(TH_WIRE, col1);
			GPU_batch_program_set_builtin(facedots, GPU_SHADER_2D_UV_FACEDOTS);
			GPU_batch_uniform_4fv(facedots, "vertColor", col1);
			GPU_batch_uniform_4fv(facedots, "selectColor", col2);
			GPU_batch_draw(facedots);
		}
	}
}

static void draw_uv_shadows_get(
        SpaceImage *sima, Object *ob, Object *obedit,
        bool *show_shadow, bool *show_texpaint)
{
	*show_shadow = *show_texpaint = false;

	if (ED_space_image_show_render(sima) || (sima->flag & SI_NO_DRAW_TEXPAINT))
		return;

	if ((sima->mode == SI_MODE_PAINT) && obedit && obedit->type == OB_MESH) {
		struct BMEditMesh *em = BKE_editmesh_from_object(obedit);

		*show_shadow = EDBM_uv_check(em);
	}

	*show_texpaint = (ob && ob->type == OB_MESH && ob->mode == OB_MODE_TEXTURE_PAINT);
}

void ED_uvedit_draw_main(
        SpaceImage *sima,
        ARegion *ar, Scene *scene, ViewLayer *view_layer, Object *obedit, Object *obact, Depsgraph *depsgraph)
{
	ToolSettings *toolsettings = scene->toolsettings;
	bool show_uvedit, show_uvshadow, show_texpaint_uvshadow;

	show_uvedit = ED_space_image_show_uvedit(sima, obedit);
	draw_uv_shadows_get(sima, obact, obedit, &show_uvshadow, &show_texpaint_uvshadow);

	if (show_uvedit || show_uvshadow || show_texpaint_uvshadow) {
		if (show_uvshadow) {
			draw_uvs_shadow(obedit);
		}
		else if (show_uvedit) {
			uint objects_len = 0;
			Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data_with_uvs(view_layer, &objects_len);
			for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
				Object *ob_iter = objects[ob_index];
				draw_uvs(sima, scene, ob_iter, depsgraph);
			}
			MEM_freeN(objects);
		}
		else {
			draw_uvs_texpaint(scene, obact);
		}

		if (show_uvedit && !(toolsettings->use_uv_sculpt))
			ED_image_draw_cursor(ar, sima->cursor);
	}
}
