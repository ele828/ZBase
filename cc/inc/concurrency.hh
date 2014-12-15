﻿#pragma once

#include <future>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <cassert>

#include "utility.hh"		/** for zforward */

namespace zbase
{

//#define zforward(_expr) std::forward<decltype(_expr)>(_expr)

/**
 * \brief 线程池。
 * \note 除非另行约定，所有公开成员函数线程安全。
 * \note 未控制线程队列的长度。
 */
class thread_pool {
private:
	template<typename _type>
	using result_of_t = typename std::result_of<_type>::type;

	template<typename _fCallable, typename... _tParams>
	using packed_task_t
        	= std::packaged_task<result_of_t<_fCallable&&(_tParams&&...)>()>;

	template<typename _fCallable, typename... _tParams>
	std::shared_ptr<packed_task_t<_fCallable&&, _tParams&&...>>
	pack_shared_task(_fCallable&& f, _tParams&&... args)
	{
        	return std::make_shared<packed_task_t<_fCallable&&, _tParams&&...>>(
                	std::bind(zforward(f), zforward(args)...));
	}

	std::vector<std::thread> workers;
	std::queue<std::function<void()>> tasks{};
	mutable std::mutex queue_mutex{};
	std::condition_variable condition{};
	/**
	 * \brief 停止状态。
	 * \note 仅在析构时设置停止。
	 */
	bool stopped = {};

public:
	/**
	 * \brief 初始化：使用指定的初始化和退出回调指定数量的工作线程。
	 * \note 若回调为空则忽略。
	 * \warning 回调的执行不提供顺序和并发安全保证。
	 */
	thread_pool(size_t, std::function<void()> = {}, std::function<void()> = {});
	/**
	 * \brief 析构：设置停止状态并等待所有执行中的线程结束。
	 * \note 断言设置停止状态时不抛出 \c std::system_error 。
	 * \note 可能阻塞。忽略每个线程阻塞的 \c std::system_error 。
	 * \note 无异常抛出：断言。
	 */
	~thread_pool() noexcept;

	/** \see wait_to_enqueue. */
	template<typename _fCallable, typename... _tParams>
	std::future<result_of_t<_fCallable&&(_tParams&&...)>>
	enqueue(_fCallable&& f, _tParams&&... args)
	{
		return wait_to_enqueue([](std::unique_lock<std::mutex>&){}, zforward(f),
			zforward(args)...);
	}

	size_t
	size() const;

	/**
	 * \warning 非线程安全。
	 */
	size_t
	size_unlocked() const noexcept
	{
		return tasks.size();
	}

	/**
	 * \brief 等待操作进入队列。
	 * \param f 准备进入队列的操作
	 * \param args 进入队列操作时的参数。
	 * \param wait 等待操作。
	 * \pre wait 调用后满足条件变量后置条件；断言：持有锁。
	 * \warning 需要确保未被停止（未进入析构），否则不保证任务被运行。
	 * \warning 使用非递归锁，等待时不能再次锁定。
	 */
	template<typename _fWaiter, typename _fCallable, typename... _tParams>
	std::future<result_of_t<_fCallable&&(_tParams&&...)>>
	wait_to_enqueue(_fWaiter wait, _fCallable&& f, _tParams&&... args)
	{
		const auto
			task(pack_shared_task(zforward(f), zforward(args)...));
		auto res(task->get_future());

		{
			std::unique_lock<std::mutex> lck(queue_mutex);

			wait(lck);
			assert(lck.owns_lock());
			tasks.push([task]{
				(*task)();
			});
		}
		condition.notify_one();
		return std::move(res);
	}
};


/**
 * \brief 任务池：带有队列大小限制的线程池。
 * \note 除非另行约定，所有公开成员函数线程安全。
 */
class task_pool : private thread_pool
{
private:
	size_t max_tasks;
	std::condition_variable enqueue_condition{};

public:
	/**
	 * \brief 初始化：使用指定的初始化和退出回调指定数量的工作线程和最大任务数。
	 * \see thread_pool::thread_pool
	 */
	task_pool(size_t n, std::function<void()> on_enter = {},
		std::function<void()> on_exit = {})
		: thread_pool(std::max<size_t>(n, 1), on_enter, on_exit),
		max_tasks(std::max<size_t>(n, 1))
	{}

	/** \warning 非线程安全. */
	bool
	can_enqueue_unlocked() const noexcept
	{
		return size_unlocked() < max_tasks;
	}

	size_t
	get_max_task_num() const noexcept
	{
		return max_tasks;
	}

	/**
	 * \brief 重置线程池。
	 * \note 阻塞等待当前所有任务完成后重新创建。
	 */
	void
	reset();

	using thread_pool::size;

	template<typename _func, typename _fCallable, typename... _tParams>
	auto
	poll(_func poller, _fCallable&& f, _tParams&&... args)
		-> decltype(enqueue(zforward(f), zforward(args)...))
	{
		return wait_to_enqueue([=](std::unique_lock<std::mutex>& lck){
			enqueue_condition.wait(lck, [=]{
				return poller() && can_enqueue_unlocked();
			});
		}, zforward(f), zforward(args)...);
	}

	template<typename _func, typename _tDuration, typename _fCallable,
		typename... _tParams>
	auto
	poll_for(_func poller, const _tDuration& duration, _fCallable&& f,
		_tParams&&... args) -> decltype(enqueue(zforward(f), zforward(args)...))
	{
		return wait_to_enqueue([=](std::unique_lock<std::mutex>& lck){
			enqueue_condition.wait_for(lck, duration, [=]{
				return poller() && can_enqueue_unlocked();
			});
		}, zforward(f), zforward(args)...);
	}

	template<typename _func, typename _tTimePoint, typename _fCallable,
		typename... _tParams>
	auto
	poll_until(_func poller, const _tTimePoint& abs_time,
		_fCallable&& f, _tParams&&... args)
		-> decltype(enqueue(zforward(f), zforward(args)...))
	{
		return wait_to_enqueue([=](std::unique_lock<std::mutex>& lck){
			enqueue_condition.wait_until(lck, abs_time, [=]{
				return poller() && can_enqueue_unlocked();
			});
		}, zforward(f), zforward(args)...);
	}
	
	template<typename _fCallable, typename... _tParams>
	inline auto
	wait(_fCallable&& f, _tParams&&... args)
		-> decltype(enqueue(zforward(f), zforward(args)...))
	{
		return poll([]{
			return true;
		}, zforward(f), zforward(args)...);
	}

	template<typename _tDuration, typename _fCallable, typename... _tParams>
	inline auto
	wait_for(const _tDuration& duration, _fCallable&& f, _tParams&&... args)
		-> decltype(enqueue(zforward(f), zforward(args)...))
	{
		return poll_for([]{
			return true;
		}, duration, zforward(f), zforward(args)...);
	}

	template<typename _fWaiter, typename _fCallable, typename... _tParams>
	auto
	wait_to_enqueue(_fWaiter wait, _fCallable&& f, _tParams&&... args)
		-> decltype(enqueue(zforward(f), zforward(args)...))
	{
		return thread_pool::wait_to_enqueue(
			[=](std::unique_lock<std::mutex>& lck){
			while(!can_enqueue_unlocked())
				wait(lck);
		}, zforward(f), zforward(args)...);
	}

	template<typename _tTimePoint, typename _fCallable, typename... _tParams>
	inline auto
	wait_until(const _tTimePoint& abs_time, _fCallable&& f, _tParams&&... args)
		-> decltype(enqueue(zforward(f), zforward(args)...))
	{
		return poll_until([]{
			return true;
		}, abs_time, zforward(f), zforward(args)...);
	}
};

} 	/**< namespace zbase */

