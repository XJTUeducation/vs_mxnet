#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <assert.h>
#include <algorithm>
#include <memory>

#include <unordered_map>

#include <VapourSynth/VapourSynth.h>
#include <VapourSynth/VSHelper.h>

#include <opencv2/core.hpp>

#ifdef _DEBUG
#pragma comment(lib, "opencv_core343d.lib")
#else
#pragma comment(lib, "opencv_core343.lib")
#endif

#pragma comment(lib, "zlib.lib")

#include "MXDll.h"

#ifdef _MSC_VER
#if defined (_WINDEF_) && defined(min) && defined(max)
#undef min
#undef max
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#define DEFER_1(x, y) x##y
#define DEFER_2(x, y) DEFER_1(x, y)
#define DEFER_0(x)    DEFER_2(x, __COUNTER__)
#define defer(expr)   auto DEFER_0(_defered_option) = deferer([&](){expr;})

template <typename Function>
struct doDefer
{
	Function f;
	doDefer(Function f) : f(f) {}
	~doDefer() { f(); }
};

template <typename Function>
doDefer<Function> deferer(Function f)
{
	return doDefer<Function>(f);
}

struct mxnetData
{
	VSNodeRef *node;
	VSVideoInfo vi;
	int patch_w, patch_h;
	int step_w, step_h;
	float scale;
	int pad, border_type;
	int output_w, output_h;
	int outstep_w, outstep_h;
	int frame_w, frame_h;
	float *srcBuffer, *dstBuffer, *padBuffer = nullptr;
	PredictorHandle hPred;
};

class BufferFile
{
public:
	std::string file_path_;
	size_t length_;
	char* buffer_;

	explicit BufferFile(std::string file_path)
		:file_path_(file_path)
	{
		std::ifstream ifs(file_path.c_str(), std::ios::in | std::ios::binary);
		if (!ifs) {
			length_ = 0;
			buffer_ = NULL;
			return;
		}

		ifs.seekg(0, std::ios::end);
		length_ = ifs.tellg();
		ifs.seekg(0, std::ios::beg);

		buffer_ = new char[sizeof(char) * length_];
		ifs.read(buffer_, length_);
		ifs.close();
	}

	size_t GetLength()
	{
		return length_;
	}
	char* GetBuffer()
	{
		return buffer_;
	}

	~BufferFile()
	{
		if (buffer_) {
			delete[] buffer_;
			buffer_ = NULL;
		}
	}
};

MXNet mx("libmxnet.dll");

template <typename T>
static inline void bitblt(T *dstp, int dst_stride, const T *srcp, int src_stride, size_t width, size_t height, size_t pad = 0)
{
	if (!height) return;

	const auto t = sizeof(T);

	if (src_stride == dst_stride && src_stride == (int)width) {
		memcpy(dstp, srcp, width * height * t);
		return;
	}

	for (size_t i = 0; i < height; i++) {
		memcpy(dstp + pad, srcp, width * t);
		srcp += src_stride;
		dstp += dst_stride;
	}
}

inline int mxForward(mxnetData * VS_RESTRICT d)
{
	int ch = d->vi.format->numPlanes;
	auto imageSize = d->patch_h * d->patch_w * ch;

	if (mx.MXPredSetInput(d->hPred, "data", d->srcBuffer, imageSize) != 0) {
		return 2;
	}

	if (mx.MXPredForward(d->hPred) != 0) {
		return 2;
	}

	mx_uint output_index = 0;

	mx_uint *shape = nullptr;
	mx_uint shape_len = 0;

	// Get Output Result
	if (mx.MXPredGetOutputShape(d->hPred, output_index, &shape, &shape_len) != 0) {
		return 2;
	}

	mx_uint outputSize = 1;
	for (mx_uint i = 0; i < shape_len; ++i) outputSize *= shape[i];

	if (outputSize != d->output_h*d->output_w*ch) {
		return 1;
	}

	if (mx.MXPredGetOutput(d->hPred, output_index, d->dstBuffer, outputSize) != 0) {
		return 2;
	}

	return 0;
}

static int process(const VSFrameRef *src, VSFrameRef *dst, mxnetData * VS_RESTRICT d, const VSAPI * vsapi) noexcept
{
	if (d->vi.format->subSamplingH || d->vi.format->subSamplingW) {
		return 3;
	}

	int ch = d->vi.format->numPlanes;
	int width = vsapi->getFrameWidth(src, 0);
	int height = vsapi->getFrameHeight(src, 0);

	float **srcp = new float *[ch];
	int *srcStride = new int[ch];
	defer(delete[] srcp; delete[] srcStride;);

	std::vector<cv::Mat> padded;
	for (int plane = 0; plane < ch; ++plane) {
		auto _srcStride = vsapi->getStride(src, plane) / sizeof(float);
		auto _srcp = reinterpret_cast<const float *>(vsapi->getReadPtr(src, plane));

		if (d->pad > 0) {
			int _width = width + d->pad * 2;
			int _height = height + d->pad * 2;

			int dststep = _width * sizeof(float);

			float *buf = d->padBuffer + _width * _height * plane;

			bitblt<float>(buf + _width * d->pad, _width, _srcp, _srcStride, width, height, d->pad);

			cv::Mat output(_height, _width, CV_32FC1, buf);
			cv::Mat input(output, cv::Rect(d->pad, d->pad, width, height));

			cv::copyMakeBorder(input, output, d->pad, d->pad, d->pad, d->pad, d->border_type | cv::BORDER_ISOLATED);

			padded.push_back(output);

			_srcp = (float *)padded[plane].data;

			if (padded[plane].isContinuous()) {
				_srcStride = width + d->pad * 2;
			} else {
				_srcStride = padded[plane].step;
			}
		}

		srcp[plane] = (float *)_srcp;
		srcStride[plane] = _srcStride;
	}

	width += d->pad * 2;
	height += d->pad * 2;

	int patchSize = d->patch_w * d->patch_h * ch;
	int outputSize = d->output_w * d->output_h * ch;

	int x = 0, y = 0;
	while (true) {
		int sy = std::min(y * d->step_h, height - d->patch_h);
		int ey = std::min(y * d->step_h + d->patch_h, height);

		while (true) {
			int sx = std::min(x * d->step_w, width - d->patch_w);
			int ex = std::min(x * d->step_w + d->patch_w, width);

			for (int plane = 0; plane < ch; ++plane) {
				auto _srcStride = srcStride[plane];
				const float *_srcp = srcp[plane] + sx + _srcStride * sy;

				mx_float *buf = d->srcBuffer + patchSize / ch * plane;
				bitblt<float>(buf, d->patch_w, _srcp, _srcStride, d->patch_w, d->patch_h);
			}

			if (auto err = mxForward(d)) return err;
			// memcpy(d->dstBuffer, d->srcBuffer, d->patch_h * d->patch_w * ch * sizeof(float));

			for (int plane = 0; plane < ch; ++plane) {
				int dstoff_x = std::min(d->frame_w - d->output_w, x * d->outstep_w);
				int dstoff_y = std::min(d->frame_h - d->output_h, y * d->outstep_h);

				const unsigned stride = vsapi->getStride(dst, plane) / sizeof(float);
				float * VS_RESTRICT dstp = reinterpret_cast<float *>(vsapi->getWritePtr(dst, plane)) + dstoff_x + dstoff_y * stride;

				float *outbuf = d->dstBuffer + outputSize / ch * plane;
				bitblt<float>(dstp, stride, outbuf, d->output_w, d->output_w, d->output_h);
			}

			if (ex == width) break;
			++x;
		}

		if (ey == height) break;
		++y;
		x = 0;
	}

	return 0;
}

static const VSFrameRef *VS_CC mxGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi)
{
	mxnetData *d = (mxnetData *)* instanceData;

	if (activationReason == arInitial) {
		vsapi->requestFrameFilter(n, d->node, frameCtx);
	} else if (activationReason == arAllFramesReady) {
		const VSFrameRef * src = vsapi->getFrameFilter(n, d->node, frameCtx);
		VSFrameRef * dst = vsapi->newVideoFrame(d->vi.format, d->vi.width, d->vi.height, src, core);

		const auto error = process(src, dst, d, vsapi);
		if (error != 0) {
			const char * err = "";

			if (error == 1)
				err = "input and target shapes do not match";
			else if (error == 2)
				err = "failed to process mxnet";
			else if (error == 3)
				err = "not support clip format";

			vsapi->setFilterError((std::string{ "mxnet: " } +err).c_str(), frameCtx);
			vsapi->freeFrame(src);
			vsapi->freeFrame(dst);
			return nullptr;
		}

		vsapi->freeFrame(src);
		return dst;
	}

	return 0;
}

static void VS_CC mxFree(void *instanceData, VSCore *core, const VSAPI *vsapi)
{
	mxnetData *d = (mxnetData *)instanceData;
	vsapi->freeNode(d->node);

	mx.MXPredFree(d->hPred);

	//delete[] d->srcBuffer;
	//delete[] d->dstBuffer;

	vs_aligned_free(d->srcBuffer);
	vs_aligned_free(d->dstBuffer);

	if (d->padBuffer)
		vs_aligned_free(d->padBuffer);
	//delete[] d->padBuffer;

	free(d);
}

static void VS_CC mxInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi)
{
	mxnetData * d = static_cast<mxnetData *>(*instanceData);
	vsapi->setVideoInfo(&d->vi, 1, node);
}

static void VS_CC mxCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi)
{
	int err;

	mxnetData d{};

	d.node = vsapi->propGetNode(in, "clip", 0, nullptr);
	d.vi = *vsapi->getVideoInfo(d.node);

	int ch = d.vi.format->numPlanes;
	int width = d.vi.width, height = d.vi.height;

	try {
		if (!isConstantFormat(&d.vi) || d.vi.format->sampleType != stFloat || d.vi.format->bitsPerSample != 32)
			throw std::string{ "only constant format 32 bit float input supported" };

		if (d.vi.format->subSamplingH || d.vi.format->subSamplingW)
			throw std::string{ "all plane must have the save size" };

		const char* symbol = vsapi->propGetData(in, "symbol", 0, &err);
		if (err)
			throw std::string{ "\"symbol\" is empty" };

		const char* param = vsapi->propGetData(in, "param", 0, &err);
		if (err)
			throw std::string{ "\"param\" is empty" };

		// Padding
		d.pad = int64ToIntS(vsapi->propGetInt(in, "padding", 0, &err));
		if (err) {
			d.pad = 0;
		} else {
			width += 2 * d.pad;
			height += 2 * d.pad;
		}

		if (d.pad) {
			d.border_type = int64ToIntS(vsapi->propGetInt(in, "boder_type", 0, &err));
			if (err)
				d.border_type = cv::BORDER_REPLICATE;

			if (!(d.border_type == cv::BORDER_CONSTANT || d.border_type == cv::BORDER_REPLICATE || d.border_type == cv::BORDER_REFLECT ||
				d.border_type == cv::BORDER_WRAP || d.border_type == cv::BORDER_REFLECT_101))
				throw std::string{ "invalid border type: check OpenCV border type for more info (default: cv::BORDER_REPLICATE)" };
		}

		// Input size
		d.patch_w = int64ToIntS(vsapi->propGetInt(in, "patch_w", 0, &err));
		if (err || d.patch_w > width || d.patch_w == 0)
			d.patch_w = width;

		d.patch_h = int64ToIntS(vsapi->propGetInt(in, "patch_h", 0, &err));
		if (err || d.patch_h > height || d.patch_h == 0)
			d.patch_h = height;

		// Step size
		d.step_w = int64ToIntS(vsapi->propGetInt(in, "step_w", 0, &err));
		if (err || d.step_w == 0)
			d.step_w = d.patch_w;

		d.step_h = int64ToIntS(vsapi->propGetInt(in, "step_h", 0, &err));
		if (err || d.step_h == 0)
			d.step_h = d.patch_h;

		if (d.step_w > width)
			d.step_w = width;

		if (d.step_h > height)
			d.step_h = height;

		// Scale
		d.scale = static_cast<float>(vsapi->propGetFloat(in, "scale", 0, &err));
		if (err)
			d.scale = 1.0;

		// Forward output size
		d.output_w = int64ToIntS(vsapi->propGetInt(in, "output_w", 0, &err));
		if (err || d.output_w == 0)
			d.output_w = d.patch_w * d.scale;

		d.output_h = int64ToIntS(vsapi->propGetInt(in, "output_h", 0, &err));
		if (err || d.output_h == 0)
			d.output_h = d.patch_h * d.scale;

		// Output frame size
		d.frame_w = int64ToIntS(vsapi->propGetInt(in, "frame_w", 0, &err));
		if (err || d.frame_w == 0)
			d.vi.width *= d.scale;
		else
			d.vi.width = d.frame_w;

		d.frame_h = int64ToIntS(vsapi->propGetInt(in, "frame_h", 0, &err));
		if (err || d.frame_h == 0)
			d.vi.height *= d.scale;
		else
			d.vi.height = d.frame_h;

		d.frame_w = d.vi.width;
		d.frame_h = d.vi.height;

		// Now d.vi is same size as output

		// Output Reconstruct step size
		d.outstep_w = int64ToIntS(vsapi->propGetInt(in, "outstep_w", 0, &err));
		if (err || d.step_w == 0)
			d.outstep_w = d.output_w;

		d.outstep_h = int64ToIntS(vsapi->propGetInt(in, "outstep_h", 0, &err));
		if (err || d.outstep_h == 0)
			d.outstep_h = d.output_h;

		if (d.outstep_w > d.vi.width)
			d.outstep_w = d.vi.width;

		if (d.outstep_h > d.vi.height)
			d.outstep_h = d.vi.height;

		// MXnet Config
		const int ctx = int64ToIntS(vsapi->propGetInt(in, "ctx", 0, &err));

		const int dev_id = int64ToIntS(vsapi->propGetInt(in, "dev_id", 0, &err));

		if (ctx != 1 && ctx != 2 && ctx != 0)
			throw std::string{ "context must be 1(cpu) or 2(gpu)" };

		if (d.patch_w < 1)
			throw std::string{ "patch_w must be greater than or equal to 1" };

		if (d.patch_h < 1)
			throw std::string{ "patch_h must be greater than or equal to 1" };

		if (d.step_w < 1)
			throw std::string{ "step_w must be greater than or equal to 1" };

		if (d.step_h < 1)
			throw std::string{ "step_h must be greater than or equal to 1" };

		if (d.output_w < 1)
			throw std::string{ "output_w must be greater than or equal to 1" };

		if (d.output_h < 1)
			throw std::string{ "output_h must be greater than or equal to 1" };

		if (d.vi.width < 1)
			throw std::string{ "frame_w must be greater than or equal to 1" };

		if (d.vi.height < 1)
			throw std::string{ "frame_h must be greater than or equal to 1" };

		if (d.outstep_w < 1)
			throw std::string{ "outstep_w must be greater than or equal to 1" };

		if (d.outstep_h < 1)
			throw std::string{ "outstep_h must be greater than or equal to 1" };

		if (dev_id < 0)
			throw std::string{ "device id must be greater than or equal to 0" };

		//d.srcBuffer = new (std::nothrow) float[d.patch_w * d.patch_h * ch];
		//d.dstBuffer = new (std::nothrow) float[d.output_w * d.output_h * ch];
		d.srcBuffer = vs_aligned_malloc<float>(d.patch_w * d.patch_h * ch * sizeof(float), 2048 * sizeof(float));
		d.dstBuffer = vs_aligned_malloc<float>(d.output_w * d.output_h * ch * sizeof(float), 2048 * sizeof(float));
		if (!d.srcBuffer || !d.dstBuffer)
			throw std::string{ "malloc failure (buffer)" };

		if (d.pad > 0) {
			//d.padBuffer = new (std::nothrow) float[(width + d.pad) * (height + d.pad)];
			d.padBuffer = vs_aligned_malloc<float>((width + d.pad) * (height + d.pad) * ch * sizeof(float), 2048 * sizeof(float));
			if (!d.padBuffer)
				throw std::string{ "malloc failure (pad buffer)" };
		}

		const std::string pluginPath{ vsapi->getPluginPath(vsapi->getPluginById("vs.kice.mxnet", core)) };
		std::string dataPath{ pluginPath.substr(0, pluginPath.find_last_of('/')) };

		BufferFile *json_data = new BufferFile(symbol);
		if (json_data->GetLength() == 0) {
			delete json_data;
			auto modelPath = dataPath + "/mxnet-symbol/" + symbol;
			json_data = new BufferFile(modelPath);
		}

		BufferFile *param_data = new BufferFile(param);
		if (param_data->GetLength() == 0) {
			delete param_data;
			auto paramPath = dataPath + "/mxnet-symbol/" + param;
			param_data = new BufferFile(paramPath);
		}

		defer([&](...) { delete json_data; delete param_data; });

		if (json_data->GetLength() == 0 || param_data->GetLength() == 0)
			throw std::string{ "Cannot open symbol json file or param data file" };

		d.hPred = 0;

		// Parameters
		int dev_type = ctx == 0 ? 1 : 2;
		mx_uint num_input_nodes = 1;
		const char* input_key[1] = { "data" };
		const char** input_keys = input_key;

		const mx_uint input_shape_indptr[] = { 0, 4 };
		const mx_uint input_shape_data[4] =
		{
			1,
			static_cast<mx_uint>(ch),
			static_cast<mx_uint>(d.patch_h),
			static_cast<mx_uint>(d.patch_w)
		};

		d.hPred = 0;

		if (!mx.IsInit()) {
			mx.LoadDll(nullptr);

			if (!mx.IsInit()) {
				throw std::string{ "Cannot load MXNet. Please install MXNet" };
			}
		}

		// Create Predictor
		if (mx.MXPredCreate(
			(const char*)json_data->GetBuffer(),
			(const char*)param_data->GetBuffer(),
			static_cast<int>(param_data->GetLength()),
			dev_type, dev_id,
			num_input_nodes,
			input_keys, input_shape_indptr, input_shape_data,
			&d.hPred) != 0) {
			throw std::string{ "Create MXNet Predictor failed" };
		}

		if (d.hPred == 0) {
			throw std::string{ "Invalid MXNet Predictor" };
		}
	} catch (const std::string & error) {
		vsapi->setError(out, ("mxnet: " + error).c_str());
		vsapi->freeNode(d.node);
		return;
	}

	mxnetData* data = new mxnetData{ d };
	vsapi->createFilter(in, out, "Predict", mxInit, mxGetFrame, mxFree, fmParallelRequests, 0, data, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin)
{
	configFunc("vs.kice.mxnet", "mx", "Use MXNet to accelerated Image-Processing in VapourSynth", VAPOURSYNTH_API_VERSION, 1, plugin);
	registerFunc("Predict",
		"clip:clip;"
		"symbol:data;"
		"param:data;"
		"patch_w:int:opt;"
		"patch_h:int:opt;"
		"scale:float:opt;"
		"output_w:int:opt;"
		"output_h:int:opt;"
		"frame_w:int:opt;"
		"frame_h:int:opt;"
		"step_w:int:opt;"
		"step_h:int:opt;"
		"outstep_w:int:opt;"
		"outstep_h:int:opt;"
		"padding:int:opt;"
		"boder_type:int:opt;"
		"ctx:int:opt;"
		"dev_id:int:opt;",
		mxCreate, nullptr, plugin);
}
