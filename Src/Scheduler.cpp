#include "Scheduler.h"
#include "Assert.h"


//TODO: Split to files. One file - one class.

namespace MT
{
	ThreadContext::ThreadContext()
		: hasNewTasksEvent(EventReset::AUTOMATIC, true)
		, state(ThreadState::ALIVE)
		, taskScheduler(nullptr)
		, thread(nullptr)
		, schedulerFiber(nullptr)
	{
	}

	ThreadContext::~ThreadContext()
	{
		ASSERT(thread == NULL, "Thread is not stopped!")
	}


	FiberContext::FiberContext()
		: activeTask(nullptr)
		, activeContext(nullptr)
		, taskStatus(FiberTaskStatus::UNKNOWN)
	{
	}

	void FiberContext::RunSubtasks(const MT::TaskDesc * taskDescArr, uint32 count)
	{
		ASSERT(activeContext, "Sanity check failed!");

		// ATTENTION !
		// copy current task description to stack.
		//  pointer to parentTask alive until all child task finished
		MT::TaskDesc parentTask = *activeTask;

		//add subtask to scheduler
		activeContext->taskScheduler->RunTasksImpl(parentTask.taskGroup, taskDescArr, count, &parentTask);

		//switch to scheduler
		MT::SwitchToFiber(activeContext->schedulerFiber);
	}


	TaskScheduler::TaskScheduler()
		: roundRobinThreadIndex(0)
	{

		//query number of processor
		threadsCount = MT::GetNumberOfHardwareThreads() - 2;
		if (threadsCount <= 0)
		{
			threadsCount = 1;
		}

		if (threadsCount > MT_MAX_THREAD_COUNT)
		{
			threadsCount = MT_MAX_THREAD_COUNT;
		}

		// create fiber pool
		for (int32 i = 0; i < MT_MAX_FIBERS_COUNT; i++)
		{
			MT::Fiber fiber = MT::CreateFiber( MT_FIBER_STACK_SIZE, FiberMain, &fiberContext[i] );
			availableFibers.Push( FiberExecutionContext(fiber, &fiberContext[i]) );
		}

		// create group done events
		for (int32 i = 0; i < TaskGroup::COUNT; i++)
		{
			groupIsDoneEvents[i].Create( MT::EventReset::MANUAL, true );
			groupCurrentlyRunningTaskCount[i].Set(0);
		}

		// create worker thread pool
		for (int32 i = 0; i < threadsCount; i++)
		{
			threadContext[i].taskScheduler = this;
			threadContext[i].thread = MT::CreateSuspendedThread( MT_SCHEDULER_STACK_SIZE, ThreadMain, &threadContext[i] );

			// bind thread to processor
			MT::SetThreadProcessor(threadContext[i].thread, i);
		}

		// run worker threads
		for (int32 i = 0; i < threadsCount; i++)
		{
			MT::ResumeThread(threadContext[i].thread);
		}
	}

	static const int THREAD_CLOSE_TIMEOUT_MS = 200;

	TaskScheduler::~TaskScheduler()
	{
		for (int32 i = 0; i < threadsCount; i++)
		{
			threadContext[i].state = ThreadState::EXIT;
			threadContext[i].hasNewTasksEvent.Signal();
		}

		for (int32 i = 0; i < threadsCount; i++)
		{
			CloseThread(threadContext[i].thread, THREAD_CLOSE_TIMEOUT_MS);
			threadContext[i].thread = NULL;
		}
	}

	MT::FiberExecutionContext TaskScheduler::RequestExecutionContext()
	{
		MT::FiberExecutionContext fiber = MT::FiberExecutionContext::Empty();
		if (!availableFibers.TryPop(fiber))
		{
			ASSERT(false, "Fibers pool is empty");
		}
		return fiber;
	}

	void TaskScheduler::ReleaseExecutionContext(MT::FiberExecutionContext fiberExecutionContext)
	{
		availableFibers.Push(fiberExecutionContext);
	}


	void TaskScheduler::ExecuteTask (MT::ThreadContext& context, MT::TaskDesc & taskDesc)
	{
		taskDesc.executionContext = context.taskScheduler->RequestExecutionContext();
		ASSERT(taskDesc.executionContext.IsValid(), "Can't get execution context from pool");

		taskDesc.executionContext.fiberContext->activeTask = &taskDesc;
		taskDesc.executionContext.fiberContext->activeContext = &context;

		MT::TaskDesc currentTask = taskDesc;
		for(;;)
		{
			ASSERT(currentTask.taskFunc != nullptr, "Invalid task function pointer");
			ASSERT(currentTask.executionContext.fiberContext, "Invalid execution context.");

			// update task status
			currentTask.executionContext.fiberContext->taskStatus = FiberTaskStatus::RUNNED;

			// run current task code
			MT::SwitchToFiber(currentTask.executionContext.fiber);

			// if task was done
			if (currentTask.executionContext.fiberContext->taskStatus == FiberTaskStatus::FINISHED)
			{
				TaskGroup::Type taskGroup = currentTask.taskGroup;

				//update group status
				int groupTaskCount = context.taskScheduler->groupCurrentlyRunningTaskCount[taskGroup].Dec();
				ASSERT(groupTaskCount >= 0, "Sanity check failed!");
				if (groupTaskCount == 0)
				{
					context.taskScheduler->groupIsDoneEvents[taskGroup].Signal();
				}

				//releasing task fiber
				context.taskScheduler->ReleaseExecutionContext(currentTask.executionContext);
				currentTask.executionContext = MT::FiberExecutionContext::Empty();

				//
				if (currentTask.parentTask != nullptr)
				{
					int childTasksCount = currentTask.parentTask->childTasksCount.Dec();
					ASSERT(childTasksCount >= 0, "Sanity check failed!");

					if (childTasksCount == 0)
					{
						// this is a last child task. restore parent task

						MT::TaskDesc * parent = currentTask.parentTask;

						// WARNING!! Thread context can changed here! Set actual current thread context.
						parent->executionContext.fiberContext->activeContext = &context;

						// copy parent to current task.
						// can't just use pointer, because parent pointer is pointer on fiber stack
						currentTask = *parent;
					} else
					{
						// child task still not finished
						// exiting
						break;
					}
				} else
				{
					// no parent task
					// exiting
					break;
				}
			} else
			{
				// current task was yielded, due to spawn subtask
				// exiting
				break;
			}

		} // while(currentTask)
	}


	void TaskScheduler::FiberMain(void* userData)
	{
		MT::FiberContext& context = *(MT::FiberContext*)(userData);

		for(;;)
		{
			ASSERT(context.activeTask, "Invalid task in fiber context");
			context.activeTask->taskFunc( context, context.activeTask->userData );
			context.taskStatus = FiberTaskStatus::FINISHED;
			MT::SwitchToFiber(context.activeContext->schedulerFiber);
		}

	}


	uint32 TaskScheduler::ThreadMain( void* userData )
	{
		ThreadContext& context = *(ThreadContext*)(userData);
		ASSERT(context.taskScheduler, "Task scheduler must be not null!");
		context.schedulerFiber = MT::ConvertCurrentThreadToFiber();

		while(context.state != ThreadState::EXIT)
		{
			MT::TaskDesc taskDesc;
			if (context.queue.TryPop(taskDesc))
			{
				//there is a new task
				ExecuteTask(context, taskDesc);
			} 
			else
			{
				//TODO: can try to steal tasks from other threads
				context.hasNewTasksEvent.Wait(2000);
			}
		}

		return 0;
	}

	void TaskScheduler::RunTasksImpl(TaskGroup::Type taskGroup, const MT::TaskDesc * taskDescArr, uint32 count, MT::TaskDesc * parentTask)
	{
		if (parentTask)
		{
			parentTask->childTasksCount.Add(count);
		}

		int startIndex = ((int)count - 1);
		for (int i = startIndex; i >= 0; i--)
		{
			ThreadContext & context = threadContext[roundRobinThreadIndex];
			roundRobinThreadIndex = (roundRobinThreadIndex + 1) % (uint32)threadsCount;

			//TODO: can be write more effective implementation here, just split to threads BEFORE submitting tasks to queue
			MT::TaskDesc desc = taskDescArr[i];
			desc.taskGroup = taskGroup;
			desc.parentTask = parentTask;

			context.queue.Push(desc);

			groupIsDoneEvents[taskGroup].Reset();
			groupCurrentlyRunningTaskCount[taskGroup].Inc();

			context.hasNewTasksEvent.Signal();
		}
	}


	void TaskScheduler::RunTasks(TaskGroup::Type taskGroup, const MT::TaskDesc * taskDescArr, uint32 count)
	{
		RunTasksImpl(taskGroup, taskDescArr, count, nullptr);
	}


	bool TaskScheduler::WaitGroup(MT::TaskGroup::Type group, uint32 milliseconds)
	{
		return groupIsDoneEvents[group].Wait(milliseconds);
	}

	bool TaskScheduler::WaitAll(uint32 milliseconds)
	{
		return Event::WaitAll(&groupIsDoneEvents[0], ARRAY_SIZE(groupIsDoneEvents), milliseconds);
	}

}