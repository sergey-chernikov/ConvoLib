#include <stdexcept>
#include <string>
#include "WorkQueue/Worker.h"
#include <string>
#include <format>

using namespace convo;

class GenericWQ : public WorkerQueue
{
public:
	std::string name() const override final { return "generic"; }
	
	static std::shared_ptr<GenericWQ> create()
	{
		return std::make_shared<GenericWQ>(1);
	}

	std::shared_ptr<WorkerQueue> makeInstance(const std::shared_ptr<InData>&) override final
	{
		return std::make_shared<GenericWQ>(++nbInstances_);
	}

	GenericWQ(unsigned nbInst) : WorkerQueue(nbInst) {}
};

WorkerPool::WorkerPool(const std::vector<std::shared_ptr<WorkerQueue>>& queues
	, const unsigned nbThreadsThreshold, const unsigned threadLimit)
	: threadLimit_((threadLimit > 0) ? threadLimit : 1), nbThreadsThreshold_(nbThreadsThreshold)
	, queues_(queues.empty() ? std::vector<std::shared_ptr<WorkerQueue>>
		{GenericWQ::create()} : queues)
{
	for (const auto& queue : queues_) {
		queue->assign(this);
	}
}

WorkerPool::~WorkerPool()
{
	cancel();
}

void WorkerPool::processQueued(const std::shared_ptr<InData>& inData
	, const Callback& cb)
{
	WorkerQueue* queue = nullptr;
	for (const auto& q : queues_) {
		if (q->accepts(inData)) {
			queue = q.get();
			break;
		}
	}
	if (!queue) {
		throw std::runtime_error("no queue accepts " + inData->name());
	}
	if (queue->needsNewInstance(inData)) {
		auto q = queue->makeInstance(inData);
		q->assign(this);
		queues_.push_back(q);
		queue = q.get();
	} else {
		queue = queue->getInstance(inData);
	}
	if (queue) {
		if (queue->needsNewThread()) {
			queue->newThread(inData);
		}
		else {
			queue->stopUnusedThreads();
		}
		queue->push({ inData, cb });
		queue->notify();
	} else {
		throw std::runtime_error(std::format("no queue instance found to handle {}"
			, inData ? inData->name() : "null msg"));
	}
}

void WorkerPool::cancel()
{
	for (const auto& q : queues_) {
		q->cancel();
	}
}

bool WorkerPool::finished() const
{
	for (const auto& q : queues_) {
		if (!q->empty()) {
			return false;
		}
	}
	return true;
}


convo::WorkerQueue::~WorkerQueue()
{
	stop();
}

void WorkerQueue::assign(WorkerPool* wp)
{
	wp_ = wp;
	threshSize_ = wp->nbThreadsThreshold_;
	limit_ = wp->threadLimit_;
}

size_t WorkerQueue::size() const
{
	std::unique_lock<std::mutex> dataLock(dataMutex_);
	return queue_.size();
}

bool WorkerQueue::needsNewThread() const
{
	unsigned nbThreads = 0;
	{
		std::lock_guard<std::mutex> lock(thrMutex_);
		nbThreads = threads_.size();
	}
	if (!nbThreads) { return true; }
	return (nbThreads < limit_) && (size() > (nbThreads + threshSize_));
}

size_t WorkerQueue::nbPending() const
{
	size_t nb = 0;
	const auto expireExec = WorkerClock::now() + procDelay_ / 2;
	{
		std::unique_lock<std::mutex> dataLock(dataMutex_);
		for (const auto& item : queue_) {
			if (!item.first) { continue; }
			if (item.first->execAfter < expireExec) { nb++; }
		}
	}
	return nb;
}

bool WorkerQueue::empty() const
{
	std::unique_lock<std::mutex> dataLock(dataMutex_);
	return queue_.empty();
}

void WorkerQueue::cancel()
{
	std::unique_lock<std::mutex> lock(cvMutex_);
	cvQueue_.wait_for(lock, std::chrono::milliseconds{1});
	cvQueue_.notify_all();
	lock.unlock();
	clear();

	std::lock_guard<std::mutex> thrLock(thrMutex_);
	for (const auto& w : workers_) {
		const auto& worker = w.lock();
		if (!worker) { continue; }
		worker->cancel();
	}
}

void WorkerQueue::stop()
{
	std::lock_guard<std::mutex> lock(thrMutex_);
	for (auto& thread : threads_) {
		if (thread.joinable()) {
			thread.join();
		}
	}
}

void convo::WorkerQueue::doProcess(const std::shared_ptr<Worker>& w
	, const std::shared_ptr<InData>& data, const Callback& cb)
{
	try {
		const auto& outResult = w->process(data);
		if (outResult.empty()) {
			return w->pushBack(data, cb);
		}
		bool retry = false;
		for (const auto& outData : outResult) {
			if (!outData) { continue; }
			else if (cb) {
				if (cb(outData) == ProcessingResult::Retry) {
					retry = true;
				}
			}
		}
		if (retry) {
			w->pushBack(data, cb);
		}
	} catch (...) {
		if (cb) {
			(void)cb(nullptr);
		}
	}
}

std::pair<std::shared_ptr<InData>, Callback> WorkerQueue::pop()
{
	std::pair<std::shared_ptr<InData>, Callback> data;
	const auto& now = WorkerClock::now();
	{
		std::unique_lock<std::mutex> dataLock(dataMutex_);
		auto it = queue_.begin();
		while (it != queue_.end()) {
			if (it->first && (it->first->execAfter > now)) {
				it++;
			} else {
				data = *it;
				it = queue_.erase(it);
				break;
			}
		}
	}
	return data;
}

#include <iostream>

void WorkerQueue::newThread(const std::shared_ptr<InData>& inData)
{
	if (!wp_) { throw std::runtime_error("no worker pool assigned"); }

	threads_.push_back(std::jthread([this, inData]
	{
		std::shared_ptr<Worker>	worker;
		try {
			worker = wp_->worker(inData);
			if (!worker) { throw std::runtime_error("is null"); }
		} catch (const std::exception& e) {
			const auto& strErr = std::format("worker creation failed: {}", e.what());
			std::cerr << strErr << "\n";
			return;
		}
		{
			std::lock_guard<std::mutex> lock(thrMutex_);
			workers_.push_back(worker);
		}
		worker->setQueue(this);
		while (worker->isRunning()) {
			const auto nbPendingItems = nbPending();
			if (nbPendingItems == 0) {
				wait();
			} else if (nbPendingItems > 10000000) {
				clear();
				if (worker->isRunning()) {
					worker->cancel();
				}
				throw std::runtime_error("worker doesn't manage with processing - "
					+ std::to_string(nbPendingItems) + " items accumulated");
			}
			if (!worker->isRunning()) {
				break;
			}
			if (!worker->isReady()) {
				std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
				continue;
			}
			const auto& data = pop();
			if (!data.first) {
				continue;
			}
			if (!worker->isRunning()) {
				break;
			}
			nbProcessing_++;
			doProcess(worker, data.first, data.second);
			nbProcessing_--;
			if (empty() && (nbProcessing_ < 2) && (threads_.size() > 1)) {
				worker->cancel();
			}
		}
	}));
}

void WorkerQueue::stopUnusedThreads()
{
	std::lock_guard<std::mutex> lock(thrMutex_);
	if ((threads_.size() > 1) && ((size() + threshSize_) < limit_)) {
		auto itThr = threads_.begin();
		int i = 0;
		while ((threads_.size() > 1) && (i < workers_.size())) {
			const auto& worker = workers_.at(i).lock();
			if (!worker || !worker->isRunning()) {
				auto itThr = threads_.begin() + i;
				if (itThr->joinable()) {
					itThr->join();
				}
				itThr = threads_.erase(itThr);
			}
			++i;
		}
	}
}


void Worker::pushBack(const std::shared_ptr<InData>& in, const Callback& cb)
{
	if (!queue_) {
		throw std::runtime_error("no queue set");
	}
	queue_.load()->push({ in, cb });
}
