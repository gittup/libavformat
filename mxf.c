/*
 * MXF demuxer.
 * Copyright (c) 2006 SmartJog S.A., Baptiste Coudurier <baptiste dot coudurier at smartjog dot com>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * References
 * SMPTE 336M KLV Data Encoding Protocol Using Key-Length-Value
 * SMPTE 377M MXF File Format Specifications
 * SMPTE 378M Operational Pattern 1a
 * SMPTE 379M MXF Generic Container
 * SMPTE 381M Mapping MPEG Streams into the MXF Generic Container
 * SMPTE 382M Mapping AES3 and Broadcast Wave Audio into the MXF Generic Container
 * SMPTE 383M Mapping DV-DIF Data to the MXF Generic Container
 *
 * Principle
 * Search for Track numbers which will identify essence element KLV packets.
 * Search for SourcePackage which define tracks which contains Track numbers.
 * Material Package contains tracks with reference to SourcePackage tracks.
 * Search for Descriptors (Picture, Sound) which contains codec info and parameters.
 * Assign Descriptors to correct Tracks.
 *
 * Metadata reading functions read Local Tags, get InstanceUID(0x3C0A) and then resolve Strong Reference from another object.
 * Returns -1 if Strong Reference could not be resolved.
 *
 * Simple demuxer, only OP1A supported and some files might not work at all.
 * Only tracks with associated descriptors will be decoded. "Highly Desirable" SMPTE 377M D.1
 * Only descriptors with EssenceSoundCompression or PictureEssenceCoding will be taken into account. "D/req" SMPTE 377M
 */

//#define DEBUG

#include "avformat.h"

typedef uint8_t UID[16];

enum MXFPackageType {
    MaterialPackage,
    SourcePackage,
};

enum MXFStructuralComponentType {
    Timecode,
    SourceClip,
};

typedef struct MXFStructuralComponent {
    UID uid;
    UID source_package_uid;
    UID data_definition_ul;
    int64_t duration;
    int64_t start_position;
    int source_track_id;
    enum MXFStructuralComponentType type;
} MXFStructuralComponent;

typedef struct MXFSequence {
    UID uid;
    UID data_definition_ul;
    MXFStructuralComponent **structural_components;
    UID *structural_components_refs;
    int structural_components_count;
    int64_t duration;
} MXFSequence;

typedef struct MXFTrack {
    UID uid;
    MXFSequence *sequence; /* mandatory, and only one */
    UID sequence_ref;
    int track_id;
    uint8_t track_number[4];
    AVRational edit_rate;
} MXFTrack;

typedef struct MXFDescriptor {
    UID uid;
    UID essence_container_ul;
    UID essence_codec_ul;
    AVRational sample_rate;
    AVRational aspect_ratio;
    int width;
    int height;
    int channels;
    int bits_per_sample;
    struct MXFDescriptor **sub_descriptors;
    UID *sub_descriptors_refs;
    int sub_descriptors_count;
    int linked_track_id;
} MXFDescriptor;

typedef struct MXFPackage {
    UID uid;
    UID package_uid;
    MXFTrack **tracks;
    UID *tracks_refs;
    int tracks_count;
    MXFDescriptor *descriptor; /* only one */
    UID descriptor_ref;
    enum MXFPackageType type;
} MXFPackage;

typedef struct MXFEssenceContainerData {
    UID uid;
    UID linked_package_uid;
} MXFEssenceContainerData;

typedef struct MXFContext {
    MXFPackage **packages;
    UID *packages_refs;
    int packages_count;
    MXFEssenceContainerData **essence_container_data_sets;
    UID *essence_container_data_sets_refs;
    int essence_container_data_sets_count;
    UID *essence_containers_uls; /* Universal Labels SMPTE RP224 */
    int essence_containers_uls_count;
    UID operational_pattern_ul;
    UID content_storage_uid;
    AVFormatContext *fc;
} MXFContext;

typedef struct KLVPacket {
    UID key;
    offset_t offset;
    uint64_t length;
} KLVPacket;

typedef struct MXFCodecUL {
    UID uid;
    enum CodecID id;
} MXFCodecUL;

static const UID mxf_metadata_preface_key                  = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x2F,0x00 };
static const UID mxf_metadata_content_storage_key          = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x18,0x00 };
static const UID mxf_metadata_source_package_key           = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x37,0x00 };
static const UID mxf_metadata_material_package_key         = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x36,0x00 };
static const UID mxf_metadata_sequence_key                 = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x0F,0x00 };
static const UID mxf_metadata_source_clip_key              = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x11,0x00 };
static const UID mxf_metadata_multiple_descriptor_key      = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x44,0x00 };
static const UID mxf_metadata_generic_sound_descriptor_key = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x42,0x00 };
static const UID mxf_metadata_cdci_descriptor_key          = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x28,0x00 };
static const UID mxf_metadata_mpegvideo_descriptor_key     = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x51,0x00 };
static const UID mxf_metadata_wave_descriptor_key          = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x48,0x00 };
static const UID mxf_metadata_static_track_key             = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x3A,0x00 };
static const UID mxf_metadata_track_key                    = { 0x06,0x0e,0x2b,0x34,0x02,0x53,0x01,0x01,0x0d,0x01,0x01,0x01,0x01,0x01,0x3b,0x00 };

/* partial keys to match */
static const uint8_t mxf_header_partition_pack_key[]       = { 0x06,0x0e,0x2b,0x34,0x02,0x05,0x01,0x01,0x0d,0x01,0x02,0x01,0x01,0x02 };
static const uint8_t mxf_essence_element_key[]             = { 0x06,0x0e,0x2b,0x34,0x01,0x02,0x01,0x01,0x0d,0x01,0x03,0x01 };

#define IS_KLV_KEY(x, y) (!memcmp(x, y, sizeof(y)))

#define PRINT_KEY(x) \
do { \
    int iterpk; \
    for (iterpk = 0; iterpk < 16; iterpk++) { \
        av_log(NULL, AV_LOG_DEBUG, "%02X ", x[iterpk]); \
    } \
    av_log(NULL, AV_LOG_DEBUG, "\n"); \
} while (0); \

static int64_t klv_decode_ber_length(ByteIOContext *pb)
{
    int64_t size = 0;
    uint8_t length = get_byte(pb);
    int type = length >> 7;

    if (type) { /* long form */
        int bytes_num = length & 0x7f;
        /* SMPTE 379M 5.3.4 guarantee that bytes_num must not exceed 8 bytes */
        if (bytes_num > 8)
            return -1;
        while (bytes_num--)
            size = size << 8 | get_byte(pb);
    } else {
        size = length & 0x7f;
    }
    return size;
}

static int klv_read_packet(KLVPacket *klv, ByteIOContext *pb)
{
    klv->offset = url_ftell(pb);
    get_buffer(pb, klv->key, 16);
    klv->length = klv_decode_ber_length(pb);
    return klv->length == -1 ? -1 : 0;
}

static int mxf_get_stream_index(AVFormatContext *s, KLVPacket *klv)
{
    int i;

    for (i = 0; i < s->nb_streams; i++) {
        MXFTrack *track = s->streams[i]->priv_data;
         /* SMPTE 379M 7.3 */
        if (!memcmp(klv->key + sizeof(mxf_essence_element_key), track->track_number, sizeof(track->track_number)))
            return i;
    }
    return -1;
}

static int mxf_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    KLVPacket klv;

    while (!url_feof(&s->pb)) {
        if (klv_read_packet(&klv, &s->pb) < 0) {
            av_log(s, AV_LOG_ERROR, "error reading KLV packet\n");
            return -1;
        }
        if (IS_KLV_KEY(klv.key, mxf_essence_element_key)) {
            av_get_packet(&s->pb, pkt, klv.length);
            pkt->stream_index = mxf_get_stream_index(s, &klv);
            return pkt->stream_index == -1 ? -1 : 0;
        } else
            url_fskip(&s->pb, klv.length);
    }
    return AVERROR_IO;
}

static int mxf_read_metadata_preface(MXFContext *mxf, KLVPacket *klv)
{
    ByteIOContext *pb = &mxf->fc->pb;
    int bytes_read = 0;

    while (bytes_read < klv->length) {
        int tag = get_be16(pb);
        int size = get_be16(pb); /* SMPTE 336M Table 8 KLV specified length, 0x53 */

        switch (tag) {
        case 0x3B03:
            get_buffer(pb, mxf->content_storage_uid, 16);
            break;
        case 0x3B09:
            get_buffer(pb, mxf->operational_pattern_ul, 16);
            break;
        case 0x3B0A:
            mxf->essence_containers_uls_count = get_be32(pb);
            if (mxf->essence_containers_uls_count >= UINT_MAX / sizeof(UID))
                return -1;
            mxf->essence_containers_uls = av_malloc(mxf->essence_containers_uls_count * sizeof(UID));
            url_fskip(pb, 4); /* useless size of objects, always 16 according to specs */
            get_buffer(pb, mxf->essence_containers_uls, mxf->essence_containers_uls_count * sizeof(UID));
            break;
        default:
            url_fskip(pb, size);
        }
        bytes_read += size + 4;
    }
    return 0;
}

static int mxf_read_metadata_content_storage(MXFContext *mxf, KLVPacket *klv)
{
    ByteIOContext *pb = &mxf->fc->pb;
    int bytes_read = 0;

    while (bytes_read < klv->length) {
        int tag = get_be16(pb);
        int size = get_be16(pb); /* SMPTE 336M Table 8 KLV specified length, 0x53 */

        dprintf("tag 0x%04X, size %d\n", tag, size);
        switch (tag) {
        case 0x1901:
            mxf->packages_count = get_be32(pb);
            if (mxf->packages_count >= UINT_MAX / sizeof(UID) ||
                mxf->packages_count >= UINT_MAX / sizeof(*mxf->packages))
                return -1;
            mxf->packages_refs = av_malloc(mxf->packages_count * sizeof(UID));
            mxf->packages = av_mallocz(mxf->packages_count * sizeof(*mxf->packages));
            url_fskip(pb, 4); /* useless size of objects, always 16 according to specs */
            get_buffer(pb, mxf->packages_refs, mxf->packages_count * sizeof(UID));
            break;
        case 0x1902:
            mxf->essence_container_data_sets_count = get_be32(pb);
            if (mxf->essence_container_data_sets_count >= UINT_MAX / sizeof(UID) ||
                mxf->essence_container_data_sets_count >= UINT_MAX / sizeof(*mxf->essence_container_data_sets))
                return -1;
            mxf->essence_container_data_sets_refs = av_malloc(mxf->essence_container_data_sets_count * sizeof(UID));
            mxf->essence_container_data_sets = av_mallocz(mxf->essence_container_data_sets_count * sizeof(*mxf->essence_container_data_sets));
            url_fskip(pb, 4); /* useless size of objects, always 16 according to specs */
            get_buffer(pb, mxf->essence_container_data_sets_refs, mxf->essence_container_data_sets_count * sizeof(UID));
            break;
        default:
            url_fskip(pb, size);
        }
        bytes_read += size + 4;
    }
    return 0;
}

static int mxf_read_metadata_source_clip(MXFContext *mxf, KLVPacket *klv)
{
    ByteIOContext *pb = &mxf->fc->pb;
    MXFStructuralComponent *source_clip = av_mallocz(sizeof(*source_clip));
    int bytes_read = 0;
    int i, j, k;

    source_clip->type = SourceClip;
    while (bytes_read < klv->length) {
        int tag = get_be16(pb);
        int size = get_be16(pb); /* SMPTE 336M Table 8 KLV specified length, 0x53 */

        dprintf("tag 0x%04X, size %d\n", tag, size);
        switch (tag) {
        case 0x3C0A:
            get_buffer(pb, source_clip->uid, 16);
            break;
        case 0x0202:
            source_clip->duration = get_be64(pb);
            break;
        case 0x1201:
            source_clip->start_position = get_be64(pb);
            break;
        case 0x1101:
            /* UMID, only get last 16 bytes */
            url_fskip(pb, 16);
            get_buffer(pb, source_clip->source_package_uid, 16);
            break;
        case 0x1102:
            source_clip->source_track_id = get_be32(pb);
            break;
        default:
            url_fskip(pb, size);
        }
        bytes_read += size + 4;
    }
    for (i = 0; i < mxf->packages_count; i++) {
        if (mxf->packages[i]) {
            for (j = 0; j < mxf->packages[i]->tracks_count; j++) {
                if (mxf->packages[i]->tracks[j] && mxf->packages[i]->tracks[j]->sequence) {
                    for (k = 0; k < mxf->packages[i]->tracks[j]->sequence->structural_components_count; k++) {
                        if (!memcmp(mxf->packages[i]->tracks[j]->sequence->structural_components_refs[k], source_clip->uid, 16)) {
                            mxf->packages[i]->tracks[j]->sequence->structural_components[k] = source_clip;
                            return 0;
                        }
                    }
                }
            }
        }
    }
    return -1;
}

static int mxf_read_metadata_material_package(MXFContext *mxf, KLVPacket *klv)
{
    ByteIOContext *pb = &mxf->fc->pb;
    MXFPackage *package = av_mallocz(sizeof(*package));
    int bytes_read = 0;
    int i;

    package->type = MaterialPackage;
    while (bytes_read < klv->length) {
        int tag = get_be16(pb);
        int size = get_be16(pb); /* KLV specified by 0x53 */

        switch (tag) {
        case 0x3C0A:
            get_buffer(pb, package->uid, 16);
            break;
        case 0x4403:
            package->tracks_count = get_be32(pb);
            if (package->tracks_count >= UINT_MAX / sizeof(UID) ||
                package->tracks_count >= UINT_MAX / sizeof(*package->tracks))
                return -1;
            package->tracks_refs = av_malloc(package->tracks_count * sizeof(UID));
            package->tracks = av_mallocz(package->tracks_count * sizeof(*package->tracks));
            url_fskip(pb, 4); /* useless size of objects, always 16 according to specs */
            get_buffer(pb, package->tracks_refs, package->tracks_count * sizeof(UID));
            break;
        default:
            url_fskip(pb, size);
        }
        bytes_read += size + 4;
    }
    for (i = 0; i < mxf->packages_count; i++) {
        if (!memcmp(mxf->packages_refs[i], package->uid, 16)) {
            mxf->packages[i] = package;
            return 0;
        }
    }
    return -1;
}

static int mxf_read_metadata_track(MXFContext *mxf, KLVPacket *klv)
{
    ByteIOContext *pb = &mxf->fc->pb;
    MXFTrack *track = av_mallocz(sizeof(*track));
    int bytes_read = 0;
    int i, j;

    while (bytes_read < klv->length) {
        int tag = get_be16(pb);
        int size = get_be16(pb); /* KLV specified by 0x53 */

        dprintf("tag 0x%04X, size %d\n", tag, size);
        switch (tag) {
        case 0x3C0A:
            get_buffer(pb, track->uid, 16);
            break;
        case 0x4801:
            track->track_id = get_be32(pb);
            break;
        case 0x4804:
            get_buffer(pb, track->track_number, 4);
            break;
        case 0x4B01:
            track->edit_rate.den = get_be32(pb);
            track->edit_rate.num = get_be32(pb);
            break;
        case 0x4803:
            get_buffer(pb, track->sequence_ref, 16);
            break;
        default:
            url_fskip(pb, size);
        }
        bytes_read += size + 4;
    }
    for (i = 0; i < mxf->packages_count; i++) {
        if (mxf->packages[i]) {
            for (j = 0; j < mxf->packages[i]->tracks_count; j++) {
                if (!memcmp(mxf->packages[i]->tracks_refs[j], track->uid, 16)) {
                    mxf->packages[i]->tracks[j] = track;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int mxf_read_metadata_sequence(MXFContext *mxf, KLVPacket *klv)
{
    ByteIOContext *pb = &mxf->fc->pb;
    MXFSequence *sequence = av_mallocz(sizeof(*sequence));
    int bytes_read = 0;
    int i, j;

    while (bytes_read < klv->length) {
        int tag = get_be16(pb);
        int size = get_be16(pb); /* KLV specified by 0x53 */

        dprintf("tag 0x%04X, size %d\n", tag, size);
        switch (tag) {
        case 0x3C0A:
            get_buffer(pb, sequence->uid, 16);
            break;
        case 0x0202:
            sequence->duration = get_be64(pb);
            break;
        case 0x0201:
            get_buffer(pb, sequence->data_definition_ul, 16);
            break;
        case 0x1001:
            sequence->structural_components_count = get_be32(pb);
            if (sequence->structural_components_count >= UINT_MAX / sizeof(UID) ||
                sequence->structural_components_count >= UINT_MAX / sizeof(*sequence->structural_components))
                return -1;
            sequence->structural_components_refs = av_malloc(sequence->structural_components_count * sizeof(UID));
            sequence->structural_components = av_mallocz(sequence->structural_components_count * sizeof(*sequence->structural_components));
            url_fskip(pb, 4); /* useless size of objects, always 16 according to specs */
            get_buffer(pb, sequence->structural_components_refs, sequence->structural_components_count * sizeof(UID));
            break;
        default:
            url_fskip(pb, size);
        }
        bytes_read += size + 4;
    }
    for (i = 0; i < mxf->packages_count; i++) {
        if (mxf->packages[i]) {
            for (j = 0; j < mxf->packages[i]->tracks_count; j++) {
                if (mxf->packages[i]->tracks[j]) {
                    if (!memcmp(mxf->packages[i]->tracks[j]->sequence_ref, sequence->uid, 16)) {
                        mxf->packages[i]->tracks[j]->sequence = sequence;
                        return 0;
                    }
                }
            }
        }
    }
    return -1;
}

static int mxf_read_metadata_source_package(MXFContext *mxf, KLVPacket *klv)
{
    ByteIOContext *pb = &mxf->fc->pb;
    MXFPackage *package = av_mallocz(sizeof(*package));
    int bytes_read = 0;
    int i;

    package->type = SourcePackage;
    while (bytes_read < klv->length) {
        int tag = get_be16(pb);
        int size = get_be16(pb); /* KLV specified by 0x53 */

        dprintf("tag 0x%04X, size %d\n", tag, size);
        switch (tag) {
        case 0x3C0A:
            get_buffer(pb, package->uid, 16);
            break;
        case 0x4403:
            package->tracks_count = get_be32(pb);
            if (package->tracks_count >= UINT_MAX / sizeof(UID) ||
                package->tracks_count >= UINT_MAX / sizeof(*package->tracks))
                return -1;
            package->tracks_refs = av_malloc(package->tracks_count * sizeof(UID));
            package->tracks = av_mallocz(package->tracks_count * sizeof(*package->tracks));
            url_fskip(pb, 4); /* useless size of objects, always 16 according to specs */
            get_buffer(pb, package->tracks_refs, package->tracks_count * sizeof(UID));
            break;
        case 0x4401:
            /* UMID, only get last 16 bytes */
            url_fskip(pb, 16);
            get_buffer(pb, package->package_uid, 16);
            break;
        case 0x4701:
            get_buffer(pb, package->descriptor_ref, 16);
            break;
        default:
            url_fskip(pb, size);
        }
        bytes_read += size + 4;
    }
    for (i = 0; i < mxf->packages_count; i++) {
        if (!memcmp(mxf->packages_refs[i], package->uid, 16)) {
            mxf->packages[i] = package;
            return 0;
        }
    }
    return -1;
}

static int mxf_read_metadata_multiple_descriptor(MXFContext *mxf, KLVPacket *klv)
{
    ByteIOContext *pb = &mxf->fc->pb;
    MXFDescriptor *descriptor = av_mallocz(sizeof(*descriptor));
    int bytes_read = 0;
    int i;

    while (bytes_read < klv->length) {
        int tag = get_be16(pb);
        int size = get_be16(pb); /* KLV specified by 0x53 */

        dprintf("tag 0x%04X, size %d\n", tag, size);
        switch (tag) {
        case 0x3C0A:
            get_buffer(pb, descriptor->uid, 16);
            break;
        case 0x3F01:
            descriptor->sub_descriptors_count = get_be32(pb);
            if (descriptor->sub_descriptors_count >= UINT_MAX / sizeof(UID) ||
                descriptor->sub_descriptors_count >= UINT_MAX / sizeof(*descriptor->sub_descriptors))
                return -1;
            descriptor->sub_descriptors_refs = av_malloc(descriptor->sub_descriptors_count * sizeof(UID));
            descriptor->sub_descriptors = av_mallocz(descriptor->sub_descriptors_count * sizeof(*descriptor->sub_descriptors));
            url_fskip(pb, 4); /* useless size of objects, always 16 according to specs */
            get_buffer(pb, descriptor->sub_descriptors_refs, descriptor->sub_descriptors_count * sizeof(UID));
            break;
        default:
            url_fskip(pb, size);
        }
        bytes_read += size + 4;
    }
    for (i = 0; i < mxf->packages_count; i++) {
        if (mxf->packages[i]) {
            if (!memcmp(mxf->packages[i]->descriptor_ref, descriptor->uid, 16)) {
                mxf->packages[i]->descriptor = descriptor;
                return 0;
            }
        }
    }
    return -1;
}

static int mxf_read_metadata_generic_descriptor(MXFContext *mxf, KLVPacket *klv)
{
    ByteIOContext *pb = &mxf->fc->pb;
    MXFDescriptor *descriptor = av_mallocz(sizeof(*descriptor));
    int bytes_read = 0;
    int i, j;

    while (bytes_read < klv->length) {
        int tag = get_be16(pb);
        int size = get_be16(pb); /* KLV specified by 0x53 */

        dprintf("tag 0x%04X, size %d\n", tag, size);
        switch (tag) {
        case 0x3C0A:
            get_buffer(pb, descriptor->uid, 16);
            break;
        case 0x3004:
            get_buffer(pb, descriptor->essence_container_ul, 16);
            break;
        case 0x3006:
            descriptor->linked_track_id = get_be32(pb);
            break;
        case 0x3201: /* PictureEssenceCoding */
            get_buffer(pb, descriptor->essence_codec_ul, 16);
            break;
        case 0x3203:
            descriptor->width = get_be32(pb);
            break;
        case 0x3202:
            descriptor->height = get_be32(pb);
            break;
        case 0x320E:
            descriptor->aspect_ratio.num = get_be32(pb);
            descriptor->aspect_ratio.den = get_be32(pb);
            break;
        case 0x3D03:
            descriptor->sample_rate.num = get_be32(pb);
            descriptor->sample_rate.den = get_be32(pb);
            break;
        case 0x3D06: /* SoundEssenceCompression */
            get_buffer(pb, descriptor->essence_codec_ul, 16);
            break;
        case 0x3D07:
            descriptor->channels = get_be32(pb);
            break;
        case 0x3D01:
            descriptor->bits_per_sample = get_be32(pb);
            break;
        default:
            url_fskip(pb, size);
        }
        bytes_read += size + 4;
    }
    for (i = 0; i < mxf->packages_count; i++) {
        if (mxf->packages[i]) {
            if (!memcmp(mxf->packages[i]->descriptor_ref, descriptor->uid, 16)) {
                mxf->packages[i]->descriptor = descriptor;
                return 0;
            } else if (mxf->packages[i]->descriptor) { /* MultipleDescriptor */
                for (j = 0; j < mxf->packages[i]->descriptor->sub_descriptors_count; j++) {
                    if (!memcmp(mxf->packages[i]->descriptor->sub_descriptors_refs[j], descriptor->uid, 16)) {
                        mxf->packages[i]->descriptor->sub_descriptors[j] = descriptor;
                        return 0;
                    }
                }
            }
        }
    }
    return -1;
}

/* SMPTE RP224 http://www.smpte-ra.org/mdd/index.html */
static const UID picture_essence_track_ul = { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x01,0x01,0x03,0x02,0x02,0x01,0x00,0x00,0x00 };
static const UID sound_essence_track_ul   = { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x01,0x01,0x03,0x02,0x02,0x02,0x00,0x00,0x00 };

static const MXFCodecUL mxf_codec_uls[] = {
    /* PictureEssenceCoding */
    { { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x03,0x04,0x01,0x02,0x02,0x01,0x02,0x02,0x00 }, CODEC_ID_MPEG2VIDEO }, /* I-Frame */
    { { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x03,0x04,0x01,0x02,0x02,0x01,0x04,0x03,0x00 }, CODEC_ID_MPEG2VIDEO }, /* Long GoP */
    { { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x03,0x04,0x01,0x02,0x02,0x01,0x02,0x03,0x00 }, CODEC_ID_MPEG2VIDEO }, /* Long GoP */
    { { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x01,0x04,0x01,0x02,0x02,0x01,0x02,0x01,0x05 }, CODEC_ID_MPEG2VIDEO }, /* D-10 30Mbps PAL */
    { { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x01,0x04,0x01,0x02,0x02,0x01,0x02,0x01,0x01 }, CODEC_ID_MPEG2VIDEO }, /* D-10 50Mbps PAL */
    { { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x01,0x04,0x01,0x02,0x02,0x02,0x02,0x04,0x00 },    CODEC_ID_DVVIDEO }, /* DVCPRO50 PAL */
    { { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x01,0x04,0x01,0x02,0x02,0x02,0x02,0x02,0x00 },    CODEC_ID_DVVIDEO }, /* DVCPRO25 PAL */
    { { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x01,0x04,0x01,0x02,0x02,0x02,0x01,0x02,0x00 },    CODEC_ID_DVVIDEO }, /* DV25 IEC PAL */
    /* SoundEssenceCompression */
    { { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x01,0x04,0x02,0x02,0x01,0x7F,0x00,0x00,0x00 },  CODEC_ID_PCM_S16LE },
    { { 0x06,0x0E,0x2B,0x34,0x04,0x01,0x01,0x07,0x04,0x02,0x02,0x01,0x7E,0x00,0x00,0x00 },  CODEC_ID_PCM_S16BE }, /* From Omneon MXF file */
};

static enum CodecID mxf_get_codec_id(const MXFCodecUL *uls, UID *uid)
{
    while (uls->id != CODEC_ID_NONE) {
        if(!memcmp(uls->uid, *uid, 16))
            return uls->id;
        uls++;
    }
    return CODEC_ID_NONE;
}

static int mxf_parse_structural_metadata(MXFContext *mxf)
{
    MXFPackage *material_package = NULL;
    MXFPackage *source_package = NULL;
    int i, j, k;

    /* TODO: handle multiple material packages (OP3x) */
    for (i = 0; i < mxf->packages_count; i++) {
        if (mxf->packages[i]->type == MaterialPackage) {
            material_package = mxf->packages[i];
            break;
        }
    }
    if (!material_package) {
        av_log(mxf->fc, AV_LOG_ERROR, "no material package found\n");
        return -1;
    }

    for (i = 0; i < material_package->tracks_count; i++) {
        MXFTrack *material_track = material_package->tracks[i];
        MXFTrack *source_track = NULL;
        MXFDescriptor *descriptor = NULL;
        MXFStructuralComponent *component = NULL;
        AVStream *st;

        /* TODO: handle multiple source clips */
        for (j = 0; j < material_track->sequence->structural_components_count; j++) {
            component = material_track->sequence->structural_components[j];
            /* TODO: handle timecode component */
            if (!component || component->type != SourceClip)
                continue;

            for (k = 0; k < mxf->packages_count; k++) {
                if (!memcmp(mxf->packages[k]->package_uid, component->source_package_uid, 16)) {
                    source_package = mxf->packages[k];
                    break;
                }
            }
            if (!source_package) {
                av_log(mxf->fc, AV_LOG_ERROR, "material track %d: no corresponding source package found\n", material_track->track_id);
                break;
            }
            for (k = 0; k < source_package->tracks_count; k++) {
                if (source_package->tracks[k]->track_id == component->source_track_id) {
                    source_track = source_package->tracks[k];
                    break;
                }
            }
            if (!source_track) {
                av_log(mxf->fc, AV_LOG_ERROR, "material track %d: no corresponding source track found\n", material_track->track_id);
                break;
            }
        }
        if (!source_track)
            continue;

        st = av_new_stream(mxf->fc, source_track->track_id);
        st->priv_data = source_track;
        st->duration = component->duration;
        if (st->duration == -1)
            st->duration = AV_NOPTS_VALUE;
        st->start_time = component->start_position;
        av_set_pts_info(st, 64, material_track->edit_rate.num, material_track->edit_rate.den);
#ifdef DEBUG
        PRINT_KEY(source_track->sequence->data_definition_ul);
#endif
        if (!memcmp(source_track->sequence->data_definition_ul, picture_essence_track_ul, 16))
            st->codec->codec_type = CODEC_TYPE_VIDEO;
        else if (!memcmp(source_track->sequence->data_definition_ul, sound_essence_track_ul, 16))
            st->codec->codec_type = CODEC_TYPE_AUDIO;
        else
            st->codec->codec_type = CODEC_TYPE_DATA;

        if (source_package->descriptor) {
            if (source_package->descriptor->sub_descriptors_count > 0) { /* SourcePackage has MultipleDescriptor */
                for (j = 0; j < source_package->descriptor->sub_descriptors_count; j++) {
                    if (source_package->descriptor->sub_descriptors[j]) {
                        if (source_package->descriptor->sub_descriptors[j]->linked_track_id == source_track->track_id) {
                            descriptor = source_package->descriptor->sub_descriptors[j];
                            break;
                        }
                    }
                }
            } else {
                descriptor = source_package->descriptor;
            }
        }
        if (!descriptor) {
            av_log(mxf->fc, AV_LOG_INFO, "source track %d: stream %d, no descriptor found\n", source_track->track_id, st->index);
            continue;
        }
#ifdef DEBUG
        PRINT_KEY(descriptor->essence_codec_ul);
#endif
        st->codec->codec_id = mxf_get_codec_id(mxf_codec_uls, &descriptor->essence_codec_ul);
        if (st->codec->codec_type == CODEC_TYPE_VIDEO) {
            st->codec->width = descriptor->width;
            st->codec->height = descriptor->height;
        } else if (st->codec->codec_type == CODEC_TYPE_AUDIO) {
            st->codec->channels = descriptor->channels;
            st->codec->bits_per_sample = descriptor->bits_per_sample;
            st->codec->sample_rate = descriptor->sample_rate.num / descriptor->sample_rate.den;
            /* TODO: implement CODEC_ID_RAWAUDIO */
            if (st->codec->codec_id == CODEC_ID_PCM_S16LE) {
                if (descriptor->bits_per_sample == 24)
                    st->codec->codec_id = CODEC_ID_PCM_S24LE;
                else if (descriptor->bits_per_sample == 32)
                    st->codec->codec_id = CODEC_ID_PCM_S32LE;
            } else if (st->codec->codec_id == CODEC_ID_PCM_S16BE) {
                if (descriptor->bits_per_sample == 24)
                    st->codec->codec_id = CODEC_ID_PCM_S24BE;
                else if (descriptor->bits_per_sample == 32)
                    st->codec->codec_id = CODEC_ID_PCM_S32BE;
            }
        }
    }
    return 0;
}

static int mxf_read_header(AVFormatContext *s, AVFormatParameters *ap)
{
    MXFContext *mxf = s->priv_data;
    KLVPacket klv;
    int ret = 0;

    mxf->fc = s;
    while (!url_feof(&s->pb)) {
        if (klv_read_packet(&klv, &s->pb) < 0) {
            av_log(s, AV_LOG_ERROR, "error reading KLV packet\n");
            return -1;
        }
        if (IS_KLV_KEY(klv.key, mxf_metadata_track_key))
            ret = mxf_read_metadata_track(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_static_track_key))
            ret = mxf_read_metadata_track(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_source_package_key))
            ret = mxf_read_metadata_source_package(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_sequence_key))
            ret = mxf_read_metadata_sequence(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_material_package_key))
            ret = mxf_read_metadata_material_package(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_source_clip_key))
            ret = mxf_read_metadata_source_clip(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_multiple_descriptor_key))
            ret = mxf_read_metadata_multiple_descriptor(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_wave_descriptor_key))
            ret = mxf_read_metadata_generic_descriptor(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_mpegvideo_descriptor_key))
            ret = mxf_read_metadata_generic_descriptor(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_cdci_descriptor_key))
            ret = mxf_read_metadata_generic_descriptor(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_generic_sound_descriptor_key))
            ret = mxf_read_metadata_generic_descriptor(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_preface_key))
            ret = mxf_read_metadata_preface(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_metadata_content_storage_key))
            ret = mxf_read_metadata_content_storage(mxf, &klv);
        else if (IS_KLV_KEY(klv.key, mxf_essence_element_key)) {
            /* FIXME avoid seek */
            url_fseek(&s->pb, klv.offset, SEEK_SET);
            break;
        } else
            url_fskip(&s->pb, klv.length);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "error reading header metadata\n");
            return ret;
        }
    }
    return mxf_parse_structural_metadata(mxf);
}

static int mxf_read_close(AVFormatContext *s)
{
    MXFContext *mxf = s->priv_data;
    int i, j, k;

    for (i = 0; i < mxf->packages_count; i++) {
        for (j = 0; j < mxf->packages[i]->tracks_count; j++) {
            for (k = 0; k < mxf->packages[i]->tracks[j]->sequence->structural_components_count; k++)
                av_freep(&mxf->packages[i]->tracks[j]->sequence->structural_components[k]);
            av_freep(&mxf->packages[i]->tracks[j]->sequence->structural_components_refs);
            av_freep(&mxf->packages[i]->tracks[j]->sequence->structural_components);
            av_freep(&mxf->packages[i]->tracks[j]->sequence);
            av_freep(&mxf->packages[i]->tracks[j]);
        }
        av_freep(&mxf->packages[i]->tracks_refs);
        av_freep(&mxf->packages[i]->tracks);
        if (mxf->packages[i]->descriptor) {
            for (k = 0; k < mxf->packages[i]->descriptor->sub_descriptors_count; k++)
                av_freep(&mxf->packages[i]->descriptor->sub_descriptors[k]);
            av_freep(&mxf->packages[i]->descriptor->sub_descriptors_refs);
            av_freep(&mxf->packages[i]->descriptor->sub_descriptors);
        }
        av_freep(&mxf->packages[i]->descriptor);
        av_freep(&mxf->packages[i]);
    }
    av_freep(&mxf->packages_refs);
    av_freep(&mxf->packages);
    for (i = 0; i < mxf->essence_container_data_sets_count; i++)
        av_freep(&mxf->essence_container_data_sets[i]);
    av_freep(&mxf->essence_container_data_sets_refs);
    av_freep(&mxf->essence_container_data_sets);
    av_freep(&mxf->essence_containers_uls);
    return 0;
}

static int mxf_probe(AVProbeData *p) {
    /* KLV packet describing MXF header partition pack */
    if (p->buf_size < sizeof(mxf_header_partition_pack_key))
        return 0;

    if (IS_KLV_KEY(p->buf, mxf_header_partition_pack_key))
        return AVPROBE_SCORE_MAX;
    else
        return 0;
}


AVInputFormat mxf_demuxer = {
    "mxf",
    "MXF format",
    sizeof(MXFContext),
    mxf_probe,
    mxf_read_header,
    mxf_read_packet,
    mxf_read_close,
    NULL,
};
