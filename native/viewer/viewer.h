#pragma once

#include "ufbx.h"
#include "external/umath.h"
#include <stdint.h>

typedef struct vi_scene vi_scene;

typedef struct vi_target {
	uint32_t target_index;
	uint32_t width;
	uint32_t height;
} vi_target;

typedef struct vi_desc {
	um_vec3 camera_pos;
	um_vec3 camera_target;
} vi_desc;

void vi_setup();
void vi_shutdown();

vi_scene *vi_make_scene(const ufbx_scene *fbx_scene);
void vi_free_scene(vi_scene *scene);

void vi_render(vi_scene *scene, const vi_target *target, const vi_desc *desc);
void vi_present(uint32_t target_index);