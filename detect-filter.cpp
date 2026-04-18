#include "detect-filter.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/core/version.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <obs-module.h>
#include <media-io/video-io.h>

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(v) (void)(v)
#endif

#define S_MODEL_PATH "model_path"
#define S_CONF "conf_threshold"
#define S_NMS "nms_threshold"
#define S_EVERY_N "process_every_n"
#define S_MAX_W "max_infer_width"
#define S_BLUR_SIZE "blur_size"
#define S_PAD_PCT "padding_percent"
#define S_MIN_AREA_PCT "min_area_percent"
#define S_DEBUG_BOXES "debug_show_boxes"
#define S_DELAY_FRAMES "delay_frames"
#define S_PLATE_REGION "plate_region"
#define S_USER_REGION_LABEL "user_region_label"

/* =============================================================================
 * Regional license plate profiles — extend here and in locale (en-US.ini)
 *
 * Each row is one entry in the "License plate region" dropdown (same order).
 * - use_shape_filter: if true, drop detections whose box aspect ratio
 *   (width / height) is outside [aspect_w_over_h_min, aspect_w_over_h_max].
 * - min_area_percent_override: if >= 0, use this as Min area % for the region;
 *   if < 0, the user's "Min area %" slider is used (Custom always uses slider).
 * Typical aspects: NA wide ~2:1–3.5:1; EU long ~4.5:1; JP often ~2:1 (car).
 * Tune for your camera FOV; false positives usually need wider max_aspect.
 * ============================================================================= */
struct PlateRegionProfile {
	bool use_shape_filter;
	float aspect_w_over_h_min;
	float aspect_w_over_h_max;
	float min_area_percent_override;
};

enum PlateRegion : int {
	PLATE_REGION_CUSTOM = 0,
	PLATE_REGION_NORTH_AMERICA,
	PLATE_REGION_EUROPE,
	PLATE_REGION_UNITED_KINGDOM,
	PLATE_REGION_JAPAN_KOREA,
	PLATE_REGION_AUSTRALASIA,
	PLATE_REGION_LATIN_AMERICA,
	PLATE_REGION_MIDDLE_EAST_AFRICA,
	PLATE_REGION_INDIA,
	PLATE_REGION_COUNT
};

static const PlateRegionProfile kPlateRegionProfiles[] = {
	{false, 0.f, 0.f, -1.f},
	{true, 1.65f, 5.2f, 0.04f},
	{true, 1.9f, 6.0f, 0.035f},
	{true, 2.2f, 7.0f, 0.04f},
	{true, 1.45f, 4.5f, 0.05f},
	{true, 1.85f, 5.5f, 0.04f},
	{true, 1.55f, 5.0f, 0.035f},
	{true, 1.4f, 5.5f, 0.03f},
	{true, 1.7f, 4.8f, 0.045f},
};

static_assert((int)PLATE_REGION_COUNT == (int)(sizeof(kPlateRegionProfiles) / sizeof(kPlateRegionProfiles[0])),
	      "kPlateRegionProfiles must match PlateRegion enum order/count");

static const PlateRegionProfile &plate_region_profile_for(int idx)
{
	if (idx < 0 || idx >= PLATE_REGION_COUNT)
		return kPlateRegionProfiles[PLATE_REGION_CUSTOM];
	return kPlateRegionProfiles[idx];
}

static bool plate_rect_matches_region_shape(const cv::Rect &r, const PlateRegionProfile &prof)
{
	if (!prof.use_shape_filter || r.height < 1)
		return true;
	const float ar = (float)r.width / (float)r.height;
	return ar >= prof.aspect_w_over_h_min && ar <= prof.aspect_w_over_h_max;
}

struct plate_blur_data {
	obs_source_t *context{};
	std::mutex mutex;
	cv::dnn::Net net;
	bool net_loaded = false;
	std::string model_path;
	float conf_threshold = 0.35f;
	float nms_threshold = 0.45f;
	int process_every_n = 1;
	int max_infer_width = 640;
	int blur_size = 31;
	int padding_percent = 12;
	float min_area_percent = 0.02f;
	bool debug_show_boxes = false;
	int delay_frames{};
	int plate_region = PLATE_REGION_CUSTOM;

	std::vector<cv::Rect> cached_boxes;
	int frame_counter = 0;

	std::atomic<uint64_t> last_log_ns{0};

	std::deque<obs_source_frame *> delay_queue;
	bool delay_queue_dims_valid = false;
	uint32_t queue_frame_width = 0;
	uint32_t queue_frame_height = 0;
	enum video_format queue_frame_format = VIDEO_FORMAT_I420;
	bool logged_delay_info = false;
};

static void plate_blur_clear_delay_queue(plate_blur_data *filter)
{
	for (obs_source_frame *fr : filter->delay_queue) {
		obs_source_frame_destroy(fr);
	}
	filter->delay_queue.clear();
	filter->delay_queue_dims_valid = false;
}

static bool frame_to_bgr(struct obs_source_frame *frame, cv::Mat &out_bgr)
{
	switch (frame->format) {
	case VIDEO_FORMAT_NV12: {
		cv::Mat y((int)frame->height, (int)frame->width, CV_8UC1, frame->data[0], (size_t)frame->linesize[0]);
		cv::Mat uv((int)frame->height / 2, (int)frame->width / 2, CV_8UC2, frame->data[1], (size_t)frame->linesize[1]);
#if CV_VERSION_MAJOR >= 4
		cv::cvtColorTwoPlane(y, uv, out_bgr, cv::COLOR_YUV2BGR_NV12);
		return true;
#else
		(void)y;
		(void)uv;
		return false;
#endif
	}
	case VIDEO_FORMAT_BGRA: {
		cv::Mat bgra((int)frame->height, (int)frame->width, CV_8UC4, frame->data[0], (size_t)frame->linesize[0]);
		cv::cvtColor(bgra, out_bgr, cv::COLOR_BGRA2BGR);
		return true;
	}
	case VIDEO_FORMAT_RGBA: {
		cv::Mat rgba((int)frame->height, (int)frame->width, CV_8UC4, frame->data[0], (size_t)frame->linesize[0]);
		cv::cvtColor(rgba, out_bgr, cv::COLOR_RGBA2BGR);
		return true;
	}
	case VIDEO_FORMAT_I420: {
		const int w = (int)frame->width;
		const int h = (int)frame->height;
		std::vector<uint8_t> buf((size_t)w * (size_t)h * 3 / 2);
		for (int row = 0; row < h; row++)
			memcpy(buf.data() + (size_t)row * w, frame->data[0] + (size_t)row * frame->linesize[0], (size_t)w);
		const int cw = w / 2;
		const int ch = h / 2;
		uint8_t *u_base = buf.data() + (size_t)w * h;
		uint8_t *v_base = u_base + (size_t)cw * ch;
		for (int row = 0; row < ch; row++) {
			memcpy(u_base + (size_t)row * cw, frame->data[1] + (size_t)row * frame->linesize[1], (size_t)cw);
			memcpy(v_base + (size_t)row * cw, frame->data[2] + (size_t)row * frame->linesize[2], (size_t)cw);
		}
		cv::Mat i420((int)(h * 3 / 2), w, CV_8UC1, buf.data());
		cv::cvtColor(i420, out_bgr, cv::COLOR_YUV2BGR_I420);
		return true;
	}
	default:
		return false;
	}
}

/** Solid black (luma 0, chroma neutral) for supported YUV/RGB packed formats. */
static void plate_blur_frame_fill_black(struct obs_source_frame *f)
{
	if (!f)
		return;
	switch (f->format) {
	case VIDEO_FORMAT_I420: {
		for (uint32_t row = 0; row < f->height; row++)
			memset(f->data[0] + (size_t)row * f->linesize[0], 0, (size_t)f->width);
		const uint32_t cw = f->width / 2;
		const uint32_t ch = f->height / 2;
		for (uint32_t row = 0; row < ch; row++) {
			memset(f->data[1] + (size_t)row * f->linesize[1], 128, (size_t)cw);
			memset(f->data[2] + (size_t)row * f->linesize[2], 128, (size_t)cw);
		}
		return;
	}
	case VIDEO_FORMAT_NV12: {
		for (uint32_t row = 0; row < f->height; row++)
			memset(f->data[0] + (size_t)row * f->linesize[0], 0, (size_t)f->width);
		for (uint32_t row = 0; row < f->height / 2; row++)
			memset(f->data[1] + (size_t)row * f->linesize[1], 128, (size_t)f->width);
		return;
	}
	case VIDEO_FORMAT_BGRA: {
		for (uint32_t row = 0; row < f->height; row++) {
			uint8_t *p = f->data[0] + (size_t)row * f->linesize[0];
			for (uint32_t x = 0; x < f->width; x++) {
				p[x * 4 + 0] = 0;
				p[x * 4 + 1] = 0;
				p[x * 4 + 2] = 0;
				p[x * 4 + 3] = 255;
			}
		}
		return;
	}
	case VIDEO_FORMAT_RGBA: {
		for (uint32_t row = 0; row < f->height; row++) {
			uint8_t *p = f->data[0] + (size_t)row * f->linesize[0];
			for (uint32_t x = 0; x < f->width; x++) {
				p[x * 4 + 0] = 0;
				p[x * 4 + 1] = 0;
				p[x * 4 + 2] = 0;
				p[x * 4 + 3] = 255;
			}
		}
		return;
	}
	default:
		return;
	}
}

/** Black frame matching \p ref (same format, size, timestamp). Caller destroys. */
static struct obs_source_frame *plate_blur_black_frame_like(const struct obs_source_frame *ref)
{
	if (!ref)
		return nullptr;
	struct obs_source_frame *out = obs_source_frame_create(ref->format, ref->width, ref->height);
	if (!out)
		return nullptr;
	out->timestamp = ref->timestamp;
	plate_blur_frame_fill_black(out);
	return out;
}

static bool bgr_to_frame_format(const cv::Mat &bgr, struct obs_source_frame *dst)
{
	switch (dst->format) {
	case VIDEO_FORMAT_NV12: {
		cv::Mat nv12;
		cv::cvtColor(bgr, nv12, cv::COLOR_BGR2YUV_NV12);
		const uint32_t w = dst->width;
		const uint32_t h = dst->height;
		for (uint32_t row = 0; row < h; row++)
			memcpy(dst->data[0] + (size_t)row * dst->linesize[0], nv12.data + (size_t)row * w, (size_t)w);
		const uint8_t *uv_src = nv12.data + (size_t)w * h;
		for (uint32_t row = 0; row < h / 2; row++)
			memcpy(dst->data[1] + (size_t)row * dst->linesize[1], uv_src + (size_t)row * w, (size_t)w);
		return true;
	}
	case VIDEO_FORMAT_BGRA: {
		cv::Mat bgra;
		cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
		for (uint32_t row = 0; row < dst->height; row++)
			memcpy(dst->data[0] + (size_t)row * dst->linesize[0], bgra.data + (size_t)row * bgra.step[0],
			       (size_t)dst->width * 4);
		return true;
	}
	case VIDEO_FORMAT_RGBA: {
		cv::Mat rgba;
		cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
		for (uint32_t row = 0; row < dst->height; row++)
			memcpy(dst->data[0] + (size_t)row * dst->linesize[0], rgba.data + (size_t)row * rgba.step[0],
			       (size_t)dst->width * 4);
		return true;
	}
	case VIDEO_FORMAT_I420: {
		cv::Mat i420;
		cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
		const int w = (int)dst->width;
		const int h = (int)dst->height;
		const int cw = w / 2;
		const int ch = h / 2;
		for (int row = 0; row < h; row++)
			memcpy(dst->data[0] + (size_t)row * dst->linesize[0], i420.data + (size_t)row * w, (size_t)w);
		const uint8_t *u_src = i420.data + (size_t)w * h;
		const uint8_t *v_src = u_src + (size_t)cw * ch;
		for (int row = 0; row < ch; row++) {
			memcpy(dst->data[1] + (size_t)row * dst->linesize[1], u_src + (size_t)row * cw, (size_t)cw);
			memcpy(dst->data[2] + (size_t)row * dst->linesize[2], v_src + (size_t)row * cw, (size_t)cw);
		}
		return true;
	}
	default:
		return false;
	}
}

struct Letterbox {
	float scale = 1.f;
	int pad_x = 0;
	int pad_y = 0;
	int net_w = 640;
	int net_h = 640;
};

static cv::Mat letterbox_bgr(const cv::Mat &src, int target, Letterbox &lb)
{
	lb.net_w = target;
	lb.net_h = target;
	float r = std::min((float)target / (float)src.cols, (float)target / (float)src.rows);
	lb.scale = r;
	int nw = (int)std::round(src.cols * r);
	int nh = (int)std::round(src.rows * r);
	cv::Mat resized;
	cv::resize(src, resized, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
	lb.pad_x = (target - nw) / 2;
	lb.pad_y = (target - nh) / 2;
	cv::Mat out(target, target, CV_8UC3, cv::Scalar(114, 114, 114));
	resized.copyTo(out(cv::Rect(lb.pad_x, lb.pad_y, nw, nh)));
	return out;
}

static void map_box_to_original(float &x1, float &y1, float &x2, float &y2, const Letterbox &lb, int orig_w, int orig_h)
{
	x1 = (x1 - (float)lb.pad_x) / lb.scale;
	y1 = (y1 - (float)lb.pad_y) / lb.scale;
	x2 = (x2 - (float)lb.pad_x) / lb.scale;
	y2 = (y2 - (float)lb.pad_y) / lb.scale;
	x1 = std::max(0.f, std::min(x1, (float)orig_w - 1.f));
	x2 = std::max(0.f, std::min(x2, (float)orig_w - 1.f));
	y1 = std::max(0.f, std::min(y1, (float)orig_h - 1.f));
	y2 = std::max(0.f, std::min(y2, (float)orig_h - 1.f));
}

struct Det {
	float x1, y1, x2, y2;
	int cls;
	float score;
};

static float index_chw(const cv::Mat &t, int /*b*/, int c, int i)
{
	return t.at<float>(0, c, i);
}

static float index_nlc(const cv::Mat &t, int /*b*/, int c, int i)
{
	return t.at<float>(0, i, c);
}

static void decode_yolov8_like(const cv::Mat &tensor, float conf_thr, const Letterbox &lb, int orig_w, int orig_h,
			       std::vector<Det> &dets)
{
	if (tensor.dims != 3 || tensor.size[0] != 1)
		return;
	const int d1 = tensor.size[1];
	const int d2 = tensor.size[2];
	const bool chw_layout = d1 < d2;
	const int nc = chw_layout ? (d1 - 4) : (d2 - 4);
	const int n = chw_layout ? d2 : d1;
	if (nc <= 0 || n <= 0)
		return;

	auto at = [&](int c, int i) -> float { return chw_layout ? index_chw(tensor, 0, c, i) : index_nlc(tensor, 0, c, i); };

	dets.clear();
	dets.reserve((size_t)n / 20);

	for (int i = 0; i < n; i++) {
		float cx = at(0, i);
		float cy = at(1, i);
		float bw = at(2, i);
		float bh = at(3, i);
		if (cx <= 1.f && cy <= 1.f && bw <= 1.f && bh <= 1.f) {
			const float s = (float)lb.net_w;
			cx *= s;
			cy *= s;
			bw *= s;
			bh *= s;
		}
		int best_c = -1;
		float best_s = 0.f;
		for (int c = 0; c < nc; c++) {
			float score = at(4 + c, i);
			if (score > best_s) {
				best_s = score;
				best_c = c;
			}
		}
		if (best_c < 0 || best_s < conf_thr)
			continue;
		float x1 = cx - bw * 0.5f;
		float y1 = cy - bh * 0.5f;
		float x2 = cx + bw * 0.5f;
		float y2 = cy + bh * 0.5f;
		map_box_to_original(x1, y1, x2, y2, lb, orig_w, orig_h);
		dets.push_back(Det{x1, y1, x2, y2, best_c, best_s});
	}
}

static void nms(std::vector<Det> &dets, float nms_thr)
{
	std::sort(dets.begin(), dets.end(), [](const Det &a, const Det &b) { return a.score > b.score; });
	std::vector<Det> keep;
	std::vector<bool> removed(dets.size(), false);
	auto iou = [](const Det &a, const Det &b) {
		float xx1 = std::max(a.x1, b.x1);
		float yy1 = std::max(a.y1, b.y1);
		float xx2 = std::min(a.x2, b.x2);
		float yy2 = std::min(a.y2, b.y2);
		float w = std::max(0.f, xx2 - xx1);
		float h = std::max(0.f, yy2 - yy1);
		float inter = w * h;
		float area_a = std::max(0.f, a.x2 - a.x1) * std::max(0.f, a.y2 - a.y1);
		float area_b = std::max(0.f, b.x2 - b.x1) * std::max(0.f, b.y2 - b.y1);
		return inter / (area_a + area_b - inter + 1e-6f);
	};
	for (size_t i = 0; i < dets.size(); i++) {
		if (removed[i])
			continue;
		keep.push_back(dets[i]);
		for (size_t j = i + 1; j < dets.size(); j++) {
			if (removed[j])
				continue;
			if (dets[i].cls != dets[j].cls)
				continue;
			if (iou(dets[i], dets[j]) > nms_thr)
				removed[j] = true;
		}
	}
	dets.swap(keep);
}

static cv::Rect expand_and_clip_rect(int x1, int y1, int x2, int y2, int pad_pct, int fw, int fh)
{
	int w = x2 - x1;
	int h = y2 - y1;
	int pad = (int)std::lround((double)std::max(w, h) * (double)pad_pct / 100.0);
	x1 -= pad;
	y1 -= pad;
	x2 += pad;
	y2 += pad;
	x1 = std::max(0, std::min(x1, fw - 1));
	y1 = std::max(0, std::min(y1, fh - 1));
	x2 = std::max(0, std::min(x2, fw));
	y2 = std::max(0, std::min(y2, fh));
	if (x2 <= x1 + 1 || y2 <= y1 + 1)
		return cv::Rect();
	return cv::Rect(x1, y1, x2 - x1, y2 - y1);
}

static void blur_plates(cv::Mat &bgr, const std::vector<cv::Rect> &boxes, int blur_ksize)
{
	int k = blur_ksize | 1;
	if (k < 3)
		k = 3;
	for (const cv::Rect &r : boxes) {
		if (r.area() <= 0)
			continue;
		cv::Mat patch = bgr(r).clone();
		cv::GaussianBlur(patch, patch, cv::Size(k, k), 0.0);
		patch.copyTo(bgr(r));
	}
}

static const char *plate_blur_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("LicensePlateBlurFilter");
}

static void plate_blur_update(void *data, obs_data_t *settings)
{
	auto *filter = static_cast<plate_blur_data *>(data);
	std::lock_guard<std::mutex> lock(filter->mutex);

	const std::string prev_model = filter->model_path;
	const int prev_delay = filter->delay_frames;

	const char *mp = obs_data_get_string(settings, S_MODEL_PATH);
	filter->model_path = mp ? mp : "";
	filter->conf_threshold = (float)obs_data_get_double(settings, S_CONF);
	filter->nms_threshold = (float)obs_data_get_double(settings, S_NMS);
	filter->process_every_n = (int)obs_data_get_int(settings, S_EVERY_N);
	if (filter->process_every_n < 1)
		filter->process_every_n = 1;
	filter->max_infer_width = (int)obs_data_get_int(settings, S_MAX_W);
	if (filter->max_infer_width < 320)
		filter->max_infer_width = 320;
	if (filter->max_infer_width > 1920)
		filter->max_infer_width = 1920;
	filter->delay_frames = (int)obs_data_get_int(settings, S_DELAY_FRAMES);
	if (filter->delay_frames < 0)
		filter->delay_frames = 0;
	if (filter->delay_frames > 300)
		filter->delay_frames = 300;

	if (filter->model_path != prev_model || filter->delay_frames != prev_delay)
		plate_blur_clear_delay_queue(filter);
	if (filter->delay_frames == 0)
		filter->logged_delay_info = false;

	filter->blur_size = (int)obs_data_get_int(settings, S_BLUR_SIZE);
	if (filter->blur_size < 3)
		filter->blur_size = 3;
	if (filter->blur_size > 151)
		filter->blur_size = 151;
	filter->padding_percent = (int)obs_data_get_int(settings, S_PAD_PCT);
	if (filter->padding_percent < 0)
		filter->padding_percent = 0;
	if (filter->padding_percent > 60)
		filter->padding_percent = 60;
	filter->min_area_percent = (float)obs_data_get_double(settings, S_MIN_AREA_PCT);
	if (filter->min_area_percent < 0.f)
		filter->min_area_percent = 0.f;
	if (filter->min_area_percent > 5.f)
		filter->min_area_percent = 5.f;
	filter->debug_show_boxes = obs_data_get_bool(settings, S_DEBUG_BOXES);

	filter->plate_region = (int)obs_data_get_int(settings, S_PLATE_REGION);
	if (filter->plate_region < 0 || filter->plate_region >= PLATE_REGION_COUNT)
		filter->plate_region = PLATE_REGION_CUSTOM;

	filter->net_loaded = false;
	filter->net = cv::dnn::Net();
	filter->cached_boxes.clear();

	if (filter->model_path.empty())
		return;
	try {
		filter->net = cv::dnn::readNetFromONNX(filter->model_path);
		filter->net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
		filter->net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
		filter->net_loaded = true;
	} catch (const cv::Exception &e) {
		blog(LOG_ERROR, "[license-plate-blur] Failed to load ONNX: %s", e.what());
	}
}

static void *plate_blur_create(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new plate_blur_data();
	filter->context = source;
	plate_blur_update(filter, settings);
	return filter;
}

static void plate_blur_destroy(void *data)
{
	auto *filter = static_cast<plate_blur_data *>(data);
	plate_blur_clear_delay_queue(filter);
	delete filter;
}

static void plate_blur_get_defaults(obs_data_t *settings)
{
	obs_module_t *mod = obs_current_module();
	if (mod) {
		char *bundled = obs_module_file(mod, "models/license_plate.onnx");
		if (bundled) {
			if (std::filesystem::exists(bundled))
				obs_data_set_default_string(settings, S_MODEL_PATH, bundled);
			bfree(bundled);
		}
	}
	obs_data_set_default_double(settings, S_CONF, 0.35);
	obs_data_set_default_double(settings, S_NMS, 0.45);
	obs_data_set_default_int(settings, S_EVERY_N, 2);
	obs_data_set_default_int(settings, S_MAX_W, 640);
	obs_data_set_default_int(settings, S_BLUR_SIZE, 31);
	obs_data_set_default_int(settings, S_PAD_PCT, 12);
	obs_data_set_default_double(settings, S_MIN_AREA_PCT, 0.05);
	obs_data_set_default_bool(settings, S_DEBUG_BOXES, false);
	obs_data_set_default_int(settings, S_DELAY_FRAMES, 0);
	obs_data_set_default_int(settings, S_PLATE_REGION, PLATE_REGION_CUSTOM);
	obs_data_set_default_string(settings, S_USER_REGION_LABEL, "");
}

static obs_properties_t *plate_blur_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_property_t *model_prop =
		obs_properties_add_path(p, S_MODEL_PATH, obs_module_text("ModelPath"), OBS_PATH_FILE, "*.onnx", nullptr);
	obs_property_set_long_description(model_prop, obs_module_text("ModelHint"));

	obs_property_t *region_prop =
		obs_properties_add_list(p, S_PLATE_REGION, obs_module_text("PlateRegion"), OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(region_prop, obs_module_text("RegionCustom"), PLATE_REGION_CUSTOM);
	obs_property_list_add_int(region_prop, obs_module_text("RegionNorthAmerica"), PLATE_REGION_NORTH_AMERICA);
	obs_property_list_add_int(region_prop, obs_module_text("RegionEurope"), PLATE_REGION_EUROPE);
	obs_property_list_add_int(region_prop, obs_module_text("RegionUnitedKingdom"), PLATE_REGION_UNITED_KINGDOM);
	obs_property_list_add_int(region_prop, obs_module_text("RegionJapanKorea"), PLATE_REGION_JAPAN_KOREA);
	obs_property_list_add_int(region_prop, obs_module_text("RegionAustralasia"), PLATE_REGION_AUSTRALASIA);
	obs_property_list_add_int(region_prop, obs_module_text("RegionLatinAmerica"), PLATE_REGION_LATIN_AMERICA);
	obs_property_list_add_int(region_prop, obs_module_text("RegionMiddleEastAfrica"), PLATE_REGION_MIDDLE_EAST_AFRICA);
	obs_property_list_add_int(region_prop, obs_module_text("RegionIndia"), PLATE_REGION_INDIA);
	obs_property_set_long_description(region_prop, obs_module_text("PlateRegionHint"));

	obs_property_t *label_prop =
		obs_properties_add_text(p, S_USER_REGION_LABEL, obs_module_text("UserRegionLabel"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(label_prop, obs_module_text("UserRegionLabelHint"));

	obs_properties_add_float_slider(p, S_CONF, obs_module_text("ConfThreshold"), 0.05, 0.95, 0.01);
	obs_properties_add_float_slider(p, S_NMS, obs_module_text("NmsThreshold"), 0.1, 0.9, 0.01);
	obs_properties_add_int_slider(p, S_EVERY_N, obs_module_text("ProcessEveryN"), 1, 30, 1);
	obs_properties_add_int_slider(p, S_MAX_W, obs_module_text("MaxInferWidth"), 320, 1920, 32);
	obs_property_t *delay_prop =
		obs_properties_add_int_slider(p, S_DELAY_FRAMES, obs_module_text("DelayFrames"), 0, 300, 1);
	obs_property_set_long_description(delay_prop, obs_module_text("DelayFramesHint"));
	obs_properties_add_int_slider(p, S_BLUR_SIZE, obs_module_text("BlurSize"), 3, 151, 2);
	obs_properties_add_int_slider(p, S_PAD_PCT, obs_module_text("PaddingPercent"), 0, 60, 1);
	obs_properties_add_float_slider(p, S_MIN_AREA_PCT, obs_module_text("MinAreaPercent"), 0.0, 2.0, 0.01);
	obs_properties_add_bool(p, S_DEBUG_BOXES, obs_module_text("DebugShowBoxes"));
	return p;
}

/** When \p force_infer_each_frame is true (delay-line mode), boxes are not stored in the cross-frame cache. */
static struct obs_source_frame *plate_blur_process_frame(plate_blur_data *filter, struct obs_source_frame *frame,
							bool force_infer_each_frame)
{
	if (!frame)
		return frame;

	cv::Mat bgr;
	if (!frame_to_bgr(frame, bgr) || bgr.empty())
		return frame;

	const int fw = bgr.cols;
	const int fh = bgr.rows;
	const float frame_area = (float)std::max(1, fw * fh);

	const int n = ++filter->frame_counter;
	const bool run_infer = force_infer_each_frame ? true : ((n % filter->process_every_n) == 0);

	std::vector<cv::Rect> boxes_copy;
	int blur_sz = 31;
	bool debug = false;

	{
		std::lock_guard<std::mutex> lock(filter->mutex);
		blur_sz = filter->blur_size;
		debug = filter->debug_show_boxes;
		const PlateRegionProfile &reg = plate_region_profile_for(filter->plate_region);
		const float min_area_pct_eff =
			(reg.min_area_percent_override >= 0.f) ? reg.min_area_percent_override : filter->min_area_percent;
		const float min_area = min_area_pct_eff / 100.f * frame_area;
		const int pad_pct = filter->padding_percent;

		std::vector<cv::Rect> immediate_boxes;
		std::vector<cv::Rect> *out_boxes = force_infer_each_frame ? &immediate_boxes : &filter->cached_boxes;

		if (run_infer && filter->net_loaded) {
			try {
				Letterbox lb;
				const int tw = filter->max_infer_width;
				cv::Mat net_img = letterbox_bgr(bgr, tw, lb);
				cv::Mat blob =
					cv::dnn::blobFromImage(net_img, 1.0 / 255.0, cv::Size(tw, tw), cv::Scalar(), true, false);
				filter->net.setInput(blob);
				cv::Mat out = filter->net.forward();
				std::vector<Det> dets;
				decode_yolov8_like(out, filter->conf_threshold, lb, fw, fh, dets);
				nms(dets, filter->nms_threshold);

				out_boxes->clear();
				for (const auto &d : dets) {
					const int x1 = (int)std::lround(d.x1);
					const int y1 = (int)std::lround(d.y1);
					const int x2 = (int)std::lround(d.x2);
					const int y2 = (int)std::lround(d.y2);
					const float a = (float)std::max(0, x2 - x1) * (float)std::max(0, y2 - y1);
					if (a < min_area)
						continue;
					cv::Rect r = expand_and_clip_rect(x1, y1, x2, y2, pad_pct, fw, fh);
					if (r.area() <= 0)
						continue;
					if (!plate_rect_matches_region_shape(r, reg))
						continue;
					out_boxes->push_back(r);
				}
			} catch (const cv::Exception &e) {
				blog(LOG_ERROR, "[license-plate-blur] Inference failed: %s", e.what());
				out_boxes->clear();
			}
		}

		if (force_infer_each_frame)
			boxes_copy = immediate_boxes;
		else
			boxes_copy = filter->cached_boxes;
	}

	if (boxes_copy.empty())
		return frame;

	blur_plates(bgr, boxes_copy, blur_sz);

	if (debug) {
		for (const cv::Rect &r : boxes_copy)
			cv::rectangle(bgr, r, cv::Scalar(0, 200, 255), 2);
	}

	struct obs_source_frame *out_frame = obs_source_frame_create(frame->format, frame->width, frame->height);
	if (!out_frame)
		return frame;
	obs_source_frame_copy(out_frame, frame);
	out_frame->timestamp = frame->timestamp;
	if (!bgr_to_frame_format(bgr, out_frame)) {
		obs_source_frame_destroy(out_frame);
		return frame;
	}

	const uint64_t now = frame->timestamp;
	if (!boxes_copy.empty() && (now - filter->last_log_ns.load() > 5000000000ULL)) {
		filter->last_log_ns.store(now);
		blog(LOG_INFO, "[license-plate-blur] active (%zu region(s))", boxes_copy.size());
	}

	return out_frame;
}

static struct obs_source_frame *plate_blur_filter_video(void *data, struct obs_source_frame *frame)
{
	auto *filter = static_cast<plate_blur_data *>(data);
	if (!frame)
		return frame;

	int delay_frames = 0;
	{
		std::lock_guard<std::mutex> lock(filter->mutex);
		delay_frames = filter->delay_frames;
	}

	if (delay_frames <= 0)
		return plate_blur_process_frame(filter, frame, false);

	obs_source_frame *oldest = nullptr;
	{
		std::lock_guard<std::mutex> lock(filter->mutex);
		if (filter->delay_queue_dims_valid &&
		    (frame->width != filter->queue_frame_width || frame->height != filter->queue_frame_height ||
		     frame->format != filter->queue_frame_format)) {
			plate_blur_clear_delay_queue(filter);
		}
		if (!filter->delay_queue_dims_valid) {
			filter->queue_frame_width = frame->width;
			filter->queue_frame_height = frame->height;
			filter->queue_frame_format = frame->format;
			filter->delay_queue_dims_valid = true;
		}

		obs_source_frame *incoming_copy = obs_source_frame_create(frame->format, frame->width, frame->height);
		if (!incoming_copy || !obs_source_frame_copy(incoming_copy, frame)) {
			if (incoming_copy)
				obs_source_frame_destroy(incoming_copy);
			return frame;
		}
		incoming_copy->timestamp = frame->timestamp;
		filter->delay_queue.push_back(incoming_copy);

		if ((int)filter->delay_queue.size() <= delay_frames) {
			struct obs_source_frame *black = plate_blur_black_frame_like(frame);
			return black ? black : frame;
		}

		if (!filter->logged_delay_info) {
			filter->logged_delay_info = true;
			blog(LOG_INFO,
			     "[license-plate-blur] delay line active (%d frame(s)); output lags input. "
			     "Increase \"Max infer width\" toward 1920 for full-HD detection if CPU can keep up.",
			     delay_frames);
		}

		oldest = filter->delay_queue.front();
		filter->delay_queue.pop_front();
	}

	struct obs_source_frame *out = plate_blur_process_frame(filter, oldest, true);
	if (out != oldest)
		obs_source_frame_destroy(oldest);
	return out;
}

static void plate_blur_remove(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(parent);
}

static struct obs_source_info make_plate_blur_source_info(void)
{
	struct obs_source_info info{};
	info.id = "license_plate_blur_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC;
	info.get_name = plate_blur_get_name;
	info.create = plate_blur_create;
	info.destroy = plate_blur_destroy;
	info.update = plate_blur_update;
	info.get_properties = plate_blur_properties;
	info.get_defaults = plate_blur_get_defaults;
	info.filter_video = plate_blur_filter_video;
	info.filter_remove = plate_blur_remove;
	info.icon_type = OBS_ICON_TYPE_UNKNOWN;
	info.version = 1;
	return info;
}

struct obs_source_info g_license_plate_blur_filter = make_plate_blur_source_info();
