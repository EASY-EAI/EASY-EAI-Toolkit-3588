#ifndef CAMERA_DISCOVER_H
#define CAMERA_DISCOVER_H

#define CAMERA_PATH_MAX 8
#define CAMERA_PATH_NAME_MAX 64

/* One live ISP mainpath: the video node, its ISP media device, and the
 * sensor/CIF subdevs found upstream in the media graph. */
typedef struct {
    int video_num;
    int media_num;
    char sensor[CAMERA_PATH_NAME_MAX];
    char cif[CAMERA_PATH_NAME_MAX];
} CameraPath;

/**
 * Scan /dev/media* for ISP devices whose pad0 input link from a rkcif-*
 * subdev is enabled, and return their rkisp_mainpath video nodes.
 * Paths are sorted by sensor entity name (m00, m01, ...) for stable
 * camera0/camera1 assignment across reboots.
 *
 * @return number of paths (>= 0), or -1 on invalid arguments.
 */
int camera_paths_discover(CameraPath *paths, int max_paths);

/**
 * Fill video_nums[0..need-1]. A non-negative entry is kept as an explicit
 * override; negative entries are filled from the discovered paths without
 * reusing a node. Returns 0 when every requested slot has a node.
 */
int camera_paths_assign(const CameraPath *paths, int path_count,
                        int *video_nums, int need);

#endif /* CAMERA_DISCOVER_H */
