#include "tasks.h"

#include "util.h"

Tasks g_tasks;

void Tasks::start(int workers) {
	for (int i = 0; i < workers; i++) threads_.emplace_back([this] { worker_loop(); });
}

void Tasks::stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
		work_.clear();
	}
	cv_.notify_all();
	for (auto& t : threads_) t.join();
	threads_.clear();
}

void Tasks::run(std::function<void()> work) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		work_.push_back(std::move(work));
	}
	cv_.notify_one();
}

void Tasks::post(std::function<void()> fn) {
	std::lock_guard<std::mutex> lock(ui_mutex_);
	ui_.push_back(std::move(fn));
}

void Tasks::drain() {
	std::vector<std::function<void()>> batch;
	{
		std::lock_guard<std::mutex> lock(ui_mutex_);
		batch.swap(ui_);
	}
	for (auto& fn : batch) fn();
}

size_t Tasks::pending() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return work_.size();
}

void Tasks::worker_loop() {
	while (true) {
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return stopping_ || !work_.empty(); });
			if (stopping_) return;
			job = std::move(work_.front());
			work_.pop_front();
		}
		try {
			job();
		} catch (const std::exception& e) {
			dlog("task threw: %s", e.what());
		} catch (...) {
			dlog("task threw an unknown exception");
		}
	}
}
