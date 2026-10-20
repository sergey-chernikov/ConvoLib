#include <stdexcept>
#include <string>
#include "WorkQueue/Worker.h"
#include <string>

using namespace convo;

WorkerPool::WorkerPool(const unsigned int nbThreadsThreshold
	, const unsigned int threadLimit, const std::vector<std::shared_ptr<WorkerQueue>>& queues)
	: threadLimit_((threadLimit > 0) ? threadLimit : 1), nbThreadsThreshold_(nbThreadsThreshold)
	, queues_(queues.empty() ? std::vector<std::shared_ptr<WorkerQueue>>
		{std::make_shared<WorkerQueue>()} : queues)
	, nbProcessing_(queues_.size())
{}

WorkerPool::~WorkerPool()
{
	cancel();
}

void WorkerPool::processQueued(const std::shared_ptr<InData>& inData
	, const Callback& cb)
{
	if (!inData) {
		throw std::invalid_argument("null inData passed");
	}
	std::shared_ptr<WorkerQueue> queue;
	unsigned qIndex = 0;
	for (const auto& q : queues_) {
		if (q->accepts(inData)) {
			queue = q;
			break;
		}
		qIndex++;
	}
	if (!queue) {
		throw std::runtime_error("no queue accepts " + inData->name());
	}
	{
		const auto totalSize = nbThreads();
		std::lock_guard<std::mutex> lock(threadsMutex_);
		auto& thrByQueue = threads_[qIndex];

		if (thrByQueue.empty() || (queue->size() > (thrByQueue.size() + nbThreadsThreshold_))
			&& (totalSize < threadLimit_)) {
			auto w = worker(inData);
			if (!w) {
				throw std::runtime_error("worker creation failed for " + inData->name());
			}
			newThread(w, queue, qIndex);
		}
		else {
#if 0
			auto itThr = thrByQueue.begin();
			while (itThr != thrByQueue.end()) {
				if (itThr->second->isRunning()) {
					itThr++;
				}
				else {
					if (itThr->first.joinable()) {
						itThr->first.join();
					}
					itThr = thrByQueue.erase(itThr);
				}
			}
#endif
		}
	}
	queue->push({ inData, cb });
	queue->notify();
}

void WorkerPool::cancel()
{
	{
		std::lock_guard<std::mutex> lock(threadsMutex_);
		for (const auto& t : threads_) {
			for (const auto& thread : t.second) {
				thread.second->cancel();
			}
		}
	}
	for (const auto& q : queues_) {
		q->cancel();
	}

	std::lock_guard<std::mutex> lock(threadsMutex_);
	for (auto& t : threads_) {
		for (auto& thread : t.second) {
			if (thread.first.joinable()) {
				thread.first.join();
			}
		}
	}
	threads_.clear();
}

void WorkerPool::newThread(const std::shared_ptr<Worker>& w
	, const std::shared_ptr<WorkerQueue>& queue, unsigned qIndex)
{
	threads_[qIndex].push_back({std::jthread([this, w, queue, qIndex]
	{
		w->setQueue(queue);
		while (w && w->isRunning()) {
			const auto nbPendingItems = queue->nbPending();
			if (nbPendingItems == 0) {
				queue->wait();
			}
			else if (nbPendingItems > 10000000) {
				queue->clear();
				if (w && w->isRunning()) {
					w->cancel();
				}
				throw std::runtime_error("worker doesn't manage with processing - "
					+ std::to_string(nbPendingItems) + " items accumulated");
			}
			if (!w || !w->isRunning()) {
				break;
			}
			const auto& data = queue->pop();
			if (!data.first) {
				continue;
			}
			if (!w || !w->isRunning()) {
				break;
			}
			auto& nbProcessing = nbProcessing_[qIndex];
			nbProcessing++;
			doProcess(w, data.first, data.second);
			nbProcessing--;
#if 0	//FIXME: with coroutines likely
			//std::lock_guard<std::mutex> lock(threadsMutex_);
			if (queue->empty() && (nbProcessing < 2)
				&& (threads_[qIndex].size() > nbThreadsThreshold_)) {
				w->cancel();
			}
#endif
		}
	}), w});
}

void WorkerPool::doProcess(const std::shared_ptr<Worker>& w
	, const std::shared_ptr<InData>& data, const Callback& cb)
{
	try {
		const auto& outResult = w->process(data);
		if (outResult.empty()) {
			w->pushBack(data, cb);
			return;
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
	}
	catch (...) {
		if (cb) {
			(void)cb(nullptr);
		}
	}
}

size_t WorkerPool::nbThreads() const
{
	size_t result = 0;
	std::unique_lock<std::mutex> lock(threadsMutex_);
	for (const auto& t : threads_) {
		result += t.second.size();
	}
	return result;
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


size_t WorkerQueue::size() const
{
	std::unique_lock<std::mutex> dataLock(dataMutex_);
	return queue_.size();
}

size_t WorkerQueue::nbPending() const
{
	size_t nb = 0;
	const auto& now = WorkerClock::now();
	{
		std::unique_lock<std::mutex> dataLock(dataMutex_);
		for (const auto& item : queue_) {
			if (item.first && (item.first->execAfter <= now)) {
				nb++;
			}
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

void Worker::pushBack(const std::shared_ptr<InData>& in, const Callback& cb)
{
	if (!queue_) {
		throw std::runtime_error("no queue set");
	}
	queue_->push({ in, cb });
}
