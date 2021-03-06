//==================================================
// GstreamerPipeline.h
//
//  Copyright (c) 2016 Benjamin Hepp.
//  Author: Benjamin Hepp
//  Created on: Nov 7, 2016
//==================================================

#pragma once

#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <condition_variable>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <ait/common.h>

#include "GstMetaCorrespondence.h"

#define FUNCTION_LINE_STRING (std::string(__FILE__) + " [" + std::string(__FUNCTION__) + ":" + std::to_string(__LINE__) + "]")
#define ANNOTATE_EXC(type, s) type (std::string(FUNCTION_LINE_STRING).append(": ").append(s))

template <typename T>
class GstWrapper
{
public:
	GstWrapper(T* ptr)
		: ptr_(ptr) {
	}

	GstWrapper(const GstWrapper& other) = delete;
	void operator=(const GstWrapper& other) = delete;

	GstWrapper(GstWrapper&& other) {
		ptr_ = other.ptr_;
		other.ptr_ = nullptr;
	}

	void operator=(GstWrapper&& other) {
		unref();
		ptr_ = other.ptr_;
		other.ptr_ = nullptr;
	}

	virtual ~GstWrapper() {
	}

	const T* operator()() const {
		return ptr_;
	}

	T* operator()() {
		return ptr_;
	}

	const T* get() const {
		return ptr_;
	}

	T* get() {
		return ptr_;
	}

	//protected:
	virtual void unref() = 0;

private:
	T* ptr_;
};

class GstCapsWrapper : public GstWrapper<GstCaps>
{
public:
	GstCapsWrapper(GstCaps* ptr)
		: GstWrapper<GstCaps>(ptr), caps_string_(nullptr) {
	}

	GstCapsWrapper(GstCapsWrapper&& other)
		: GstWrapper(std::move(other)) {
		caps_string_ = other.caps_string_;
		other.caps_string_ = nullptr;
	}

	void operator=(GstCapsWrapper&& other) {
		GstWrapper::operator=(std::move(other));
		caps_string_ = other.caps_string_;
	}

	~GstCapsWrapper() override {
		unref();
	}

	const gchar* getString() {
		if (caps_string_ == nullptr) {
			caps_string_ = gst_caps_to_string(get());
		}
		return caps_string_;
	}

protected:
	void unref() override {
		if (caps_string_ != nullptr) {
			g_free(caps_string_);
		}
		if (get() != nullptr) {
			gst_caps_unref(get());
		}
	}

	gchar* caps_string_;
};

class GstBufferWrapper : public GstWrapper<GstBuffer>
{
public:
	class Error : public std::runtime_error
	{
	public:
		Error(const std::string &str)
			: std::runtime_error(str) {
		}
	};

	GstBufferWrapper(GstBuffer* ptr)
		: GstWrapper<GstBuffer>(ptr), mapped_(false), mapped_writable_(false), owns_buffer_(true) {
	}

	GstBufferWrapper(GstBuffer* ptr, bool owns_buffer)
		: GstWrapper<GstBuffer>(ptr), mapped_(false), mapped_writable_(false), owns_buffer_(owns_buffer) {
	}

	GstBufferWrapper(GstBufferWrapper&& other)
		: GstWrapper(std::move(other)) {
		owns_buffer_ = other.owns_buffer_;
		info_ = other.info_;
		mapped_ = other.mapped_;
		mapped_writable_ = other.mapped_writable_;
	}

	void operator=(GstBufferWrapper&& other) {
		GstWrapper::operator=(std::move(other));
		owns_buffer_ = other.owns_buffer_;
		info_ = other.info_;
		mapped_ = other.mapped_;
	}

	~GstBufferWrapper() override {
		unref();
	}

	gsize getSize() {
		ensureMapped();
		return info_.size;
	}

	const guint8* getData() {
		ensureMapped();
		return info_.data;
	}

	guint8* getDataWritable() {
		ensureMappedWritable();
		return info_.data;
	}

	const GstMapInfo& getMapInfo() {
		ensureMapped();
		return info_;
	}

	void map() {
		if (!mapped_) {
			if (gst_buffer_map(get(), &info_, GST_MAP_READ) != TRUE) {
				throw ANNOTATE_EXC(Error, std::string("Unable to map Gstreamer buffer"));
			}
			mapped_ = true;
		}
	}

	void mapWritable() {
		if (!mapped_writable_) {
			if (mapped_) {
				unmap();
			}
			if (gst_buffer_map(get(), &info_, GST_MAP_WRITE) != TRUE) {
				throw ANNOTATE_EXC(Error, std::string("Unable to map writable Gstreamer buffer"));
			}
			mapped_ = true;
			mapped_writable_ = true;
		}
	}

	void unmap() {
		gst_buffer_unmap(get(), &info_);
		mapped_ = false;
		mapped_writable_ = false;
	}

protected:
	void unref() override {
		if (get() != nullptr) {
			if (mapped_) {
				unmap();
			}
			if (owns_buffer_) {
				gst_buffer_unref(get());
			}
		}
	}

private:
	void ensureMapped() {
		if (!mapped_) {
			map();
		}
	}

	void ensureMappedWritable() {
		if (!mapped_writable_) {
			mapWritable();
		}
	}

	GstMapInfo info_;
	bool mapped_;
	bool mapped_writable_;
	bool owns_buffer_;
};

class GstSampleWrapper : public GstWrapper<GstSample>
{
public:
	GstSampleWrapper(GstSample* ptr)
		: GstWrapper<GstSample>(ptr) {
	}

	GstSampleWrapper(GstSampleWrapper&& other)
		: GstWrapper(std::move(other)) {
	}

	void operator=(GstSampleWrapper&& other) {
		GstWrapper::operator=(std::move(other));
	}

	~GstSampleWrapper() override {
		unref();
	}

	GstBufferWrapper getBuffer() {
		GstBuffer* gst_buffer = gst_sample_get_buffer(get());
		return GstBufferWrapper(gst_buffer, false);
	}

protected:
	void unref() override {
		if (get() != nullptr) {
			gst_sample_unref(get());
		}
	}
};

template <typename T>
class SPSCFixedQueue
{
public:
	SPSCFixedQueue(unsigned int max_queue_size = 5)
		: max_queue_size_(max_queue_size), discard_everything_(false) {
	}

	void setDiscardEverything(bool discard_everything) {
		discard_everything_ = discard_everything;
	}

	unsigned int getMaxQueueSize() const {
		return max_queue_size_;
	}

	bool empty() const {
		return queue_.empty();
	}

	size_t size() const {
		return queue_.size();
	}

	void clear() {
		queue_.clear();
	}

	template <typename TMutex>
	T popFront(const std::unique_lock<TMutex>& lock) {
		if (!lock) {
			throw ANNOTATE_EXC(std::runtime_error, "Lock has not been acquired");
		}
		return popFrontWithoutLocking();
	}

	template <typename TMutex>
	T popFront(const std::lock_guard<TMutex>& lock) {
		return popFrontWithoutLocking();
	}

	T popFront() {
		std::lock_guard<std::mutex> lock(mutex_);
		return popFrontWithoutLocking();
	}

	std::mutex& getMutex() {
		return mutex_;
	}

	std::condition_variable& getQueueFilledCondition() {
		return queue_filled_condition_;
	}

	bool pushBack(T& element, bool block = false) {
		if (block) {
			std::unique_lock<std::mutex> lock(mutex_);
			while (queue_.size() >= max_queue_size_ && !discard_everything_) {
				queue_space_available_condition_.wait_for(lock, std::chrono::milliseconds(100),
					[&]() { return queue_.size() < max_queue_size_ || discard_everything_; });
			}
			if (discard_everything_) {
				return false;
			}
			queue_.push_back(std::move(element));
			queue_filled_condition_.notify_one();
			return true;
		}
		else {
			if (queue_.size() < max_queue_size_) {
				{
					std::lock_guard<std::mutex> lock(mutex_);
					queue_.push_back(std::move(element));
				}
				queue_filled_condition_.notify_one();
				return true;
			}
			else {
				return false;
			}
		}
	}

private:
	T popFrontWithoutLocking() {
		T element(std::move(queue_.front()));
		queue_.pop_front();
		queue_space_available_condition_.notify_one();
		return element;
	}

	std::deque<T> queue_;
	unsigned int max_queue_size_;
	std::mutex mutex_;
	std::condition_variable queue_filled_condition_;
	std::condition_variable queue_space_available_condition_;
	std::atomic<bool> discard_everything_;
};

template <typename TUserData>
class GstreamerPipeline;

template <typename TUserData>
class AppSrcSinkQueue : public SPSCFixedQueue<std::tuple<GstBufferWrapper, TUserData>>
{
	using ElementType = std::tuple<GstBufferWrapper, TUserData>;
	using Base = SPSCFixedQueue<ElementType>;

public:
	const unsigned int FRAME_DROP_REPORT_RATE = 10;
	const unsigned int CORRESPONDENCE_FAIL_REPORT_RATE = 5;
	const size_t MAX_USER_DATA_QUEUE_SIZE = 100;

	class Error : public std::runtime_error
	{
	public:
		Error(const std::string &str)
			: std::runtime_error(str) {
		}
	};

	enum DiscardMode {
		DISCARD_INPUT_FRAMES,
		DISCARD_OUTPUT_FRAMES,
	};

	AppSrcSinkQueue(const DiscardMode discard_mode, unsigned int max_output_queue_size = 5, unsigned int max_input_queue_size = 3)
		: Base(max_output_queue_size),
		discard_mode_(discard_mode),
		output_byte_counter_(0), input_byte_counter_(0),
		src_overflow_counter_(0), sink_overflow_counter_(0), correspondence_fail_counter_(0),
		max_input_queue_size_(max_input_queue_size) {
	}

	bool pushData(GstAppSrc* appsrc, GstBufferWrapper buffer, const TUserData& user_data) {
		AIT_ASSERT(GST_IS_BUFFER(buffer.get()));
		std::lock_guard<std::mutex> lock(user_data_queue_mutex_);
		if (discard_mode_ == DISCARD_INPUT_FRAMES && user_data_queue_.size() >= max_input_queue_size_) {
			return false;
		}

		// Increase refcount because gst_app_src_push_buffer will take ownership
		gst_buffer_ref(buffer.get());
		//        std::cout << "Pushing buffer into appsrc" << std::endl;
		GstFlowReturn ret = gst_app_src_push_buffer(appsrc, buffer.get());
		//    std::cout << "Done" << std::endl;
		if (ret == GST_FLOW_OK) {
			if (user_data_queue_.size() >= MAX_USER_DATA_QUEUE_SIZE) {
				// This is kind of hacky to ensure the user data queue is not blocking new frames
				user_data_queue_.pop_front();
				++src_overflow_counter_;
				if (src_overflow_counter_ >= FRAME_DROP_REPORT_RATE) {
					std::cout << "WARNING: AppSrcSinkQueue user data queue is full. Dropped " << FRAME_DROP_REPORT_RATE << " user data entries" << std::endl;
					src_overflow_counter_ = 0;
				}
			}

			GstreamerBufferInfo buffer_info;
			buffer_info.pts = GST_BUFFER_PTS(buffer.get());
			buffer_info.dts = GST_BUFFER_DTS(buffer.get());
			buffer_info.duration = GST_BUFFER_DURATION(buffer.get());
			buffer_info.offset = GST_BUFFER_OFFSET(buffer.get());
			buffer_info.offset_end = GST_BUFFER_OFFSET_END(buffer.get());

			user_data_queue_.push_back(std::make_tuple(buffer_info, user_data));

			// Compute input bandwidth
			input_frame_rate_counter_.count();
			double rate;
			unsigned int frame_count = input_frame_rate_counter_.getCount();
			input_byte_counter_ += buffer.getSize();
			if (input_frame_rate_counter_.reportRate(rate)) {
				double bandwidth = rate * input_byte_counter_ / static_cast<double>(frame_count) / 1024.0;
				input_byte_counter_ = 0;
				std::cout << "Pushing Gstreamer buffers into pipeline with " << rate << " Hz. Bandwidth: " << bandwidth << "kB/s" << std::endl;
			}

			return true;
		}
		else {
			return false;
		}
	}

private:
	friend class GstreamerPipeline<TUserData>;

	GstFlowReturn newSampleCallback(GstAppSink* appsink) {
		// Retrieve the sample
		GstSample* gst_sample = gst_app_sink_pull_sample(appsink);
		//g_signal_emit_by_name(appsink, "pull-sample", &gst_sample);
		if (gst_sample == nullptr) {
			if (gst_app_sink_is_eos(appsink) == TRUE) {
				std::cout << "Received EOS condition" << std::endl;
			}
			else {
				throw std::runtime_error("Unable to pull new sample from appsink");
			}
		}
		else {
			GstSampleWrapper sample(gst_sample);
			// Get buffer correspondence
			GstBufferWrapper buffer = sample.getBuffer();
#if !SIMULATE_ZED
			int correspondence_id;
			if (!gst_buffer_correspondence_meta_has(buffer.get())) {
				correspondence_id = -1;
			}
			else {
				correspondence_id = gst_buffer_correspondence_meta_get_id(buffer.get());
			}
			if (correspondence_id == -1) {
				++correspondence_fail_counter_;
				if (correspondence_fail_counter_ >= CORRESPONDENCE_FAIL_REPORT_RATE) {
					std::cout << "WARNING: Could not establish correspondence of frame and user data" << std::endl;
					correspondence_fail_counter_ = 0;
				}
				return GST_FLOW_OK;
			}
#endif
			//std::cout << "correspondence_id=" << correspondence_id << std::endl;
			// Copy buffer
			{
				GstBuffer* gst_buffer_copy = gst_buffer_copy_deep(buffer.get());
				if (gst_buffer_copy == nullptr) {
					throw ANNOTATE_EXC(Error, "Unable to copy Gstreamer buffer");
				}
				GstBufferWrapper buffer_copy(gst_buffer_copy);
				std::swap(buffer, buffer_copy);
				// Delete sample as fast as possible so that appsink has free buffers available
				GstSampleWrapper deleted_sample(std::move(sample));
			}

			// Compute output bandwidth
			output_frame_rate_counter_.count();
			double rate;
			unsigned int frame_count = output_frame_rate_counter_.getCount();
			output_byte_counter_ += buffer.getSize();
			if (output_frame_rate_counter_.reportRate(rate)) {
				double bandwidth = rate * output_byte_counter_ / static_cast<double>(frame_count) / 1024.0;
				output_byte_counter_ = 0;
				std::cout << "Outputting Gstreamer buffers with " << rate << " Hz. Bandwidth: " << bandwidth << "kB/s" << std::endl;
			}

			std::tuple<GstreamerBufferInfo, TUserData> tuple;
#if !SIMULATE_ZED
			{
				std::lock_guard<std::mutex> lock(user_data_queue_mutex_);
				if (user_data_queue_.empty()) {
					std::cerr << "ERROR: Received gstreamer sample but user data queue is empty. Discarding sample." << std::endl;
					return GST_FLOW_OK;
				}
				do {
					tuple = std::move(user_data_queue_.front());
					user_data_queue_.pop_front();
					if (correspondence_id < std::get<0>(tuple).offset) {
						throw ANNOTATE_EXC(Error, "Correspondence id is smaller than first element in user data queue");
					}
				} while (correspondence_id > std::get<0>(tuple).offset);
			}
#endif
			GstreamerBufferInfo& buffer_info = std::get<0>(tuple);
			const TUserData& user_data = std::get<1>(tuple);
			GST_BUFFER_PTS(buffer.get()) = buffer_info.pts;
			GST_BUFFER_DTS(buffer.get()) = buffer_info.dts;
			GST_BUFFER_DURATION(buffer.get()) = buffer_info.duration;
			GST_BUFFER_OFFSET(buffer.get()) = buffer_info.offset;
			GST_BUFFER_OFFSET_END(buffer.get()) = buffer_info.offset_end;

			ElementType output_tuple = std::make_tuple(std::move(buffer), user_data);
			bool block = discard_mode_ != DiscardMode::DISCARD_OUTPUT_FRAMES;
			if (!Base::pushBack(output_tuple, block)) {
				++sink_overflow_counter_;
				if (sink_overflow_counter_ >= FRAME_DROP_REPORT_RATE) {
					std::cout << "WARNING: Appsrcsink output queue is full. Dropped " << FRAME_DROP_REPORT_RATE << " frames" << std::endl;
					sink_overflow_counter_ = 0;
				}
			}
		}

		return GST_FLOW_OK;
	}

	std::deque<std::tuple<GstreamerBufferInfo, TUserData>> user_data_queue_;
	std::mutex user_data_queue_mutex_;

	ait::RateCounter output_frame_rate_counter_;
	size_t output_byte_counter_;
	ait::RateCounter input_frame_rate_counter_;
	size_t input_byte_counter_;

	unsigned int src_overflow_counter_;
	unsigned int sink_overflow_counter_;
	unsigned int correspondence_fail_counter_;

	unsigned int max_input_queue_size_;
	DiscardMode discard_mode_;
};

template <typename TUserData>
class GstreamerPipeline
{
public:
	using clock = std::chrono::system_clock;
	const unsigned int WATCHDOG_RESET_COUNT = 10;
	const std::chrono::seconds WATCHDOG_TIMEOUT = std::chrono::seconds(2);

	using OutputTupleType = std::tuple<GstBufferWrapper, TUserData>;

	GstreamerPipeline(const typename AppSrcSinkQueue<TUserData>::DiscardMode discard_mode, unsigned int max_output_queue_size = 5, unsigned int max_input_queue_size = 3)
		: pipeline_(nullptr), pipeline_state(GST_STATE_NULL),
		appsink_(nullptr), appsrc_(nullptr),
		appsrcsink_queue_(discard_mode, max_output_queue_size, max_input_queue_size),
		watchdog_counter_(0), delivering_appsink_sample_(false), frame_counter_(0) {
	}

	GstreamerPipeline(const GstreamerPipeline&) = delete;
	void operator=(const GstreamerPipeline&) = delete;

	virtual ~GstreamerPipeline() {
		if (pipeline_ != nullptr) {
			stop();
		}
		gst_object_unref(pipeline_);
	}

	void initialize() {
		if (pipeline_ != nullptr) {
			throw ANNOTATE_EXC(std::runtime_error, "Pipeline was already initialized");
		}
		appsrc_ = GST_APP_SRC(gst_element_factory_make("appsrc", "source"));
		if (appsrc_ == nullptr) {
			throw std::runtime_error("Unable to create app source element");
		}
		g_object_set(appsrc_, "stream-type", GST_APP_STREAM_TYPE_STREAM, nullptr);
		//g_object_set(appsrc_, "format", GST_FORMAT_TIME, nullptr);
		g_object_set(appsrc_, "format", GST_FORMAT_BYTES, nullptr);
		//g_object_set(appsrc_, "format", GST_FORMAT_BUFFERS, nullptr);
		g_object_set(appsrc_, "block", TRUE, nullptr);
		g_object_set(appsrc_, "max-bytes", 5000000, nullptr);

		appsink_ = GST_APP_SINK(gst_element_factory_make("appsink", "sink"));
		if (appsink_ == nullptr) {
			throw std::runtime_error("Unable to create app sink element");
		}
		g_object_set(appsink_, "emit-signals", TRUE, nullptr);
		g_object_set(appsink_, "sync", FALSE, nullptr);
		//g_object_set(appsink_, "drop", TRUE, nullptr);
		//g_object_set(appsink_, "max-buffers", 5, nullptr);
		// Connect new sample signal to AppSrcSinkQueue
		g_signal_connect(appsink_, "new-sample", G_CALLBACK(GstreamerPipeline<TUserData>::newAppsinkSampleCallbackStatic), this);

		pipeline_ = createPipeline(appsrc_, appsink_);

		std::cout << "Gstreamer pipeline initialized successfully" << std::endl;
	}

	GstPipeline* getNativePipeline() {
		ensureInitialized();
		return pipeline_;
	}

	GstAppSrc* getNativeAppSrc() {
		ensureInitialized();
		return appsrc_;
	}

	GstAppSink* getNativeAppSink() {
		ensureInitialized();
		return appsink_;
	}

	//    bool hasSamples() const {
	//        ensureInitialized();
	//        GstClockTime timeout = 0;
	//        GstSample* gst_sample = gst_app_sink_try_pull_sample(appsink_, timeout);
	//        if (gst_sample == nullptr) {
	//            if (gst_app_sink_is_eos(appsink_) == TRUE) {
	//            }
	//            return false;
	//        }
	//        return true;
	//    }

	//AppSrcSinkQueue<TUserData>& getAppSrcSinkQueue() {
	//    ensureInitialized();
	//    return appsrcsink_queue_;
	//}

	GstCapsWrapper getOutputCaps() {
		GstPad *appsink_sink_pad = gst_element_get_static_pad(GST_ELEMENT(appsink_), "sink");
		if (appsink_sink_pad == nullptr) {
			throw std::runtime_error("Unable to get appsink sink pad");
		}
		GstCaps* gst_caps = gst_pad_get_current_caps(appsink_sink_pad);
		GstCapsWrapper output_caps(gst_caps);
		return output_caps;
	}

	bool setInputCaps(const GstCapsWrapper& caps) {
		ensureInitialized();
		gst_app_src_set_caps(appsrc_, caps.get());
		return true;
	}

	bool hasOutput() const {
		return !appsrcsink_queue_.empty();
	}

	size_t getAvailableOutputSize() const {
		return appsrcsink_queue_.size();
	}

	template <typename TMutex>
	OutputTupleType popOutput(const std::unique_lock<TMutex>& lock) {
		return appsrcsink_queue_.popFront(lock);
	}

	template <typename TMutex>
	OutputTupleType popOutput(const std::lock_guard<TMutex>& lock) {
		return appsrcsink_queue_.popFront(lock);
	}

	OutputTupleType popOutput() {
		return appsrcsink_queue_.popFront();
	}

	std::mutex& getMutex() {
		return appsrcsink_queue_.getMutex();
	}

	std::condition_variable& getOutputCondition() {
		return appsrcsink_queue_.getQueueFilledCondition();
	}

	bool pushInput(GstBufferWrapper& buffer, const TUserData& user_data) {
		ensureInitialized();
		AIT_ASSERT(GST_IS_BUFFER(buffer.get()));

		clock::time_point now = clock::now();
		if (!delivering_appsink_sample_ && now - last_appsink_sample_time_ >= WATCHDOG_TIMEOUT) {
			++watchdog_counter_;
			if (watchdog_counter_ >= WATCHDOG_RESET_COUNT) {
				std::cout << "WARNING: Pipeline watchdog activated. Restarting pipeline" << std::endl;
				stop();
				start();
				return false;
			}
		}
		else if (watchdog_counter_ > 0) {
			watchdog_counter_ = 0;
		}

		// TODO: get from appsrc caps
		double frame_period = 1 / 10.;
		// Overwrite timing information to make sure that pipeline runs through
		GstClock* clock = gst_pipeline_get_clock(getNativePipeline());
		GstClockTime time_now = gst_clock_get_time(clock);
		GstClockTime gst_frame_period = static_cast<guint64>(GST_SECOND * frame_period);
		static GstClockTime time_previous = time_now - gst_frame_period;

		if (time_now - time_previous <= gst_frame_period) {
			GST_BUFFER_PTS(buffer.get()) = time_previous + gst_frame_period;
		}
		else {
			GST_BUFFER_PTS(buffer.get()) = time_now;
		}
		GST_BUFFER_DTS(buffer.get()) = GST_CLOCK_TIME_NONE;
		GST_BUFFER_DURATION(buffer.get()) = static_cast<guint64>(GST_SECOND * frame_period);
		GST_BUFFER_OFFSET(buffer.get()) = frame_counter_;
		GST_BUFFER_OFFSET_END(buffer.get()) = GST_BUFFER_OFFSET_NONE;

		// Disable timing information in buffer
		//static unsigned int buffer_counter = 0;
		//GST_BUFFER_PTS(buffer.get()) = GST_CLOCK_TIME_NONE;
		//GST_BUFFER_DTS(buffer.get()) = GST_CLOCK_TIME_NONE;
		//GST_BUFFER_DURATION(buffer.get()) = GST_CLOCK_TIME_NONE;
		//GST_BUFFER_OFFSET(buffer.get()) = buffer_counter;
		//GST_BUFFER_OFFSET_END(buffer.get()) = GST_BUFFER_OFFSET_NONE;
		//++buffer_counter;

		attachMetadataToBuffer(buffer, static_cast<int>(GST_BUFFER_OFFSET(buffer.get())));

		bool result = appsrcsink_queue_.pushData(appsrc_, std::move(buffer), user_data);
		if (result) {
			++frame_counter_;
			time_previous = time_now;
		}

		return result;
	}

	void start() {
		ensureInitialized();
		appsrcsink_queue_.clear();
		if (message_thread_.joinable()) {
			stop();
		}
		appsrcsink_queue_.setDiscardEverything(false);
		terminate_ = false;
		// Start playing
		GstStateChangeReturn ret = gst_element_set_state(GST_ELEMENT(pipeline_), GST_STATE_PLAYING);
		if (ret == GST_STATE_CHANGE_FAILURE) {
			gst_object_unref(pipeline_);
			throw std::runtime_error("Unable to set pipeline state");
		}
		watchdog_counter_ = 0;
		last_appsink_sample_time_ = clock::now();
		message_thread_ = std::thread([this]() {
			this->gstreamerLoop();
		});
	}

	void stop() {
		ensureInitialized();
		appsrcsink_queue_.setDiscardEverything(true);
		terminate_ = true;
		std::cout << "GstreamerPipeline: Setting pipeline state" << std::endl;
		gst_element_set_state(GST_ELEMENT(pipeline_), GST_STATE_NULL);
		std::cout << "GstreamerPipeline: Stopping pipeline" << std::endl;
		if (message_thread_.joinable()) {
			message_thread_.join();
		}
		std::cout << "GstreamerPipeline: Pipeline stopped" << std::endl;
	}

	GstState getState() const {
		return pipeline_state;
	}

	bool isPlaying() const {
		return pipeline_state == GST_STATE_PLAYING;
	}

	void setStateChangeCallback(const std::function<void(GstState, GstState, GstState)> callback) {
		state_change_callback_ = callback;
	}

protected:
	virtual GstPipeline* createPipeline(GstAppSrc* appsrc, GstAppSink* appsink) const = 0;

	virtual void attachMetadataToBuffer(GstBufferWrapper& buffer, int id) {
		gst_buffer_add_correspondence_meta(buffer.get(), id);
	};

	virtual void gstreamerLoop()
	{
		// Wait until error or EOS
		GstBus *bus = gst_element_get_bus(GST_ELEMENT(pipeline_));
		while (!terminate_) {
			GstMessage *msg = gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED));

			if (msg != nullptr) {
				GError *err;
				gchar *debug_info;
				switch (GST_MESSAGE_TYPE(msg)) {
				case GST_MESSAGE_ERROR:
					gst_message_parse_error(msg, &err, &debug_info);
					std::cerr << "Error received from element " << GST_OBJECT_NAME(msg->src) << ": " << err->message << std::endl;
					std::cerr << "Debugging information: " << (debug_info ? debug_info : "none") << std::endl;
					g_clear_error(&err);
					g_free(debug_info);
					terminate_ = true;
					break;
				case GST_MESSAGE_EOS:
					std::cout << "Stream finished." << std::endl;
					terminate_ = true;
					break;
				case GST_MESSAGE_STATE_CHANGED:
				{
					GstState old_state, new_state, pending_state;
					gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
					if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
						//if (new_state == GST_STATE_PLAYING) {
						//    //std::cout << "Resetting time" << std::endl;
						//    //data.start_time = std::chrono::system_clock::now();
						//    //data.timestamp = 0;
						//}
						pipeline_state = new_state;
						std::cout << "Pipeline state changed from " << gst_element_state_get_name(old_state)
							<< " to " << gst_element_state_get_name(new_state) << std::endl;
						if (state_change_callback_) {
							state_change_callback_(old_state, new_state, pending_state);
						}
					}
					break;
				}
				default:
					std::cerr << "Unexpected message received." << std::endl;
					break;
				}
				gst_message_unref(msg);
			}
		}
		gst_object_unref(bus);
	}

private:
	void ensureInitialized() const {
		if (pipeline_ == nullptr) {
			throw ANNOTATE_EXC(std::runtime_error, "Pipeline was not initialized");
		}
	}

	static GstFlowReturn newAppsinkSampleCallbackStatic(GstElement* sink, GstreamerPipeline* data) {
		return data->newAppsinkSampleCallback(GST_APP_SINK(sink));
	}

	GstFlowReturn newAppsinkSampleCallback(GstAppSink* appsink) {
		AIT_ASSERT(appsink == appsink_);
		delivering_appsink_sample_ = true;
		return appsrcsink_queue_.newSampleCallback(appsink_);
		last_appsink_sample_time_ = clock::now();
		delivering_appsink_sample_ = false;
	}

	GstPipeline* pipeline_;
	GstState pipeline_state;

	GstAppSrc* appsrc_;
	GstAppSink* appsink_;
	AppSrcSinkQueue<TUserData> appsrcsink_queue_;

	guint64 frame_counter_;
	unsigned int watchdog_counter_;
	std::chrono::time_point<clock> last_appsink_sample_time_;
	std::atomic<bool> delivering_appsink_sample_;

	std::atomic_bool terminate_;
	std::thread message_thread_;
	std::function<void(GstState, GstState, GstState)> state_change_callback_;
};
