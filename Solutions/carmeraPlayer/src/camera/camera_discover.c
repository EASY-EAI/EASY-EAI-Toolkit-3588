#define _GNU_SOURCE

#include "camera_discover.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/media.h>

#define MEDIA_SCAN_MAX 32
#define ENTITY_MAX 64

typedef struct {
    struct media_entity_desc desc;
    char name[sizeof(((struct media_entity_desc *)0)->name) + 1];
} Entity;

static void copy_name(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t n = strnlen(src, src_len);

    if (n >= dst_len)
        n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int open_media(int index, struct media_device_info *info)
{
    char path[32];
    int fd;

    snprintf(path, sizeof(path), "/dev/media%d", index);
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    memset(info, 0, sizeof(*info));
    if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, info) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int enum_entities(int fd, Entity *ents, int max)
{
    struct media_entity_desc desc;
    int count = 0;

    memset(&desc, 0, sizeof(desc));
    desc.id = MEDIA_ENT_ID_FLAG_NEXT;
    while (count < max && ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &desc) == 0) {
        ents[count].desc = desc;
        copy_name(ents[count].name, sizeof(ents[count].name),
                  desc.name, sizeof(desc.name));
        count++;
        memset(&desc, 0, sizeof(desc));
        desc.id = ents[count - 1].desc.id | MEDIA_ENT_ID_FLAG_NEXT;
    }
    return count;
}

static int find_entity(const Entity *ents, int count, const char *name)
{
    for (int i = 0; i < count; ++i)
        if (!strcmp(ents[i].name, name))
            return i;
    return -1;
}

/* Return the enabled rkcif-* subdev feeding the ISP on pad0, if any.
 * ENUM_LINKS only reports an entity's outgoing links, so the CIF->ISP link
 * must be found by walking every entity in the media device. */
static int isp_source_name(int fd, const struct media_entity_desc *isp,
                           const Entity *ents, int ent_count,
                           char *name, size_t name_len)
{
    for (int e = 0; e < ent_count; ++e) {
        const struct media_entity_desc *ent = &ents[e].desc;
        struct media_pad_desc *pads;
        struct media_link_desc *links;
        struct media_links_enum links_enum;
        int found = 0;

        if (ent->links == 0 || strncmp(ents[e].name, "rkcif-", 6) != 0)
            continue;

        pads = calloc(ent->pads ? ent->pads : 1, sizeof(*pads));
        links = calloc(ent->links, sizeof(*links));
        if (!pads || !links) {
            free(pads);
            free(links);
            return -1;
        }

        memset(&links_enum, 0, sizeof(links_enum));
        links_enum.entity = ent->id;
        links_enum.pads = pads;
        links_enum.links = links;
        if (ioctl(fd, MEDIA_IOC_ENUM_LINKS, &links_enum) == 0) {
            for (int i = 0; i < ent->links; ++i) {
                if (!(links[i].flags & MEDIA_LNK_FL_ENABLED))
                    continue;
                if (links[i].sink.entity != isp->id || links[i].sink.index != 0)
                    continue;
                snprintf(name, name_len, "%s", ents[e].name);
                found = 1;
                break;
            }
        }
        free(pads);
        free(links);
        if (found)
            return 0;
    }
    return -1;
}

static int media_matches(const struct media_device_info *info, const char *model)
{
    const char *bus = info->bus_info;

    if (!strcmp(info->model, model))
        return 1;
    if (!strncmp(bus, "platform:", 9) && !strcmp(bus + 9, model))
        return 1;
    return 0;
}

/* The ISP-side CIF subdev appears as the matching media device upstream. */
static int find_sensor_by_cif(const char *cif, char *sensor, size_t sensor_len)
{
    for (int m = 0; m < MEDIA_SCAN_MAX; ++m) {
        struct media_device_info info;
        Entity ents[ENTITY_MAX];
        int fd, count, found = -1;

        fd = open_media(m, &info);
        if (fd < 0)
            continue;
        if (!media_matches(&info, cif)) {
            close(fd);
            continue;
        }
        count = enum_entities(fd, ents, ENTITY_MAX);
        close(fd);
        for (int i = 0; i < count; ++i) {
            if (ents[i].desc.type == MEDIA_ENT_F_CAM_SENSOR) {
                found = i;
                break;
            }
        }
        if (found < 0)
            return -1;
        snprintf(sensor, sensor_len, "%s", ents[found].name);
        return 0;
    }
    return -1;
}

static int video_node_exists(int video_num)
{
    char path[32];

    snprintf(path, sizeof(path), "/dev/video%d", video_num);
    return access(path, F_OK) == 0;
}

static int path_cmp(const void *a, const void *b)
{
    const CameraPath *pa = a;
    const CameraPath *pb = b;
    int cmp = strcmp(pa->sensor, pb->sensor);

    if (cmp)
        return cmp;
    return pa->video_num - pb->video_num;
}

int camera_paths_discover(CameraPath *paths, int max_paths)
{
    int count = 0;

    if (!paths || max_paths <= 0)
        return -1;

    for (int m = 0; m < MEDIA_SCAN_MAX && count < max_paths; ++m) {
        struct media_device_info info;
        Entity ents[ENTITY_MAX];
        char cif[CAMERA_PATH_NAME_MAX];
        int fd, ent_count, isp, mainpath, video_num;

        fd = open_media(m, &info);
        if (fd < 0)
            continue;
        if (strncmp(info.driver, "rkisp", 5) != 0) {
            close(fd);
            continue;
        }

        ent_count = enum_entities(fd, ents, ENTITY_MAX);
        isp = find_entity(ents, ent_count, "rkisp-isp-subdev");
        mainpath = find_entity(ents, ent_count, "rkisp_mainpath");
        if (isp < 0 || mainpath < 0 ||
            isp_source_name(fd, &ents[isp].desc, ents, ent_count,
                            cif, sizeof(cif)) != 0) {
            close(fd);
            continue;
        }
        video_num = (int)ents[mainpath].desc.dev.minor;
        close(fd);

        if (!video_node_exists(video_num))
            continue;

        memset(&paths[count], 0, sizeof(paths[count]));
        paths[count].video_num = video_num;
        paths[count].media_num = m;
        snprintf(paths[count].cif, sizeof(paths[count].cif), "%s", cif);
        if (find_sensor_by_cif(cif, paths[count].sensor,
                               sizeof(paths[count].sensor)) != 0)
            snprintf(paths[count].sensor, sizeof(paths[count].sensor), "%s", cif);
        count++;
    }

    qsort(paths, count, sizeof(*paths), path_cmp);
    return count;
}

int camera_paths_assign(const CameraPath *paths, int path_count,
                        int *video_nums, int need)
{
    if (!video_nums || need < 0 || need > 2 || (path_count > 0 && !paths))
        return -1;

    for (int i = 0; i < need; ++i) {
        if (video_nums[i] >= 0)
            continue;
        for (int j = 0; j < path_count; ++j) {
            int used = 0;

            for (int k = 0; k < need; ++k) {
                if (k != i && video_nums[k] == paths[j].video_num) {
                    used = 1;
                    break;
                }
            }
            if (!used) {
                video_nums[i] = paths[j].video_num;
                break;
            }
        }
        if (video_nums[i] < 0)
            return -1;
    }
    return 0;
}
