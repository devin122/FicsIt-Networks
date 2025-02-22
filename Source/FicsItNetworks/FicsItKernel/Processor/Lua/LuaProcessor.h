#pragma once

#include <chrono>
#include <set>
#include <thread>

#include "FicsItKernel/Processor/Processor.h"
#include "LuaFileSystemAPI.h"

class AFINStateEEPROMLua;
struct lua_State;
struct lua_Debug;

namespace FicsItKernel {
	namespace Lua {
		class LuaFileSystemListener : public FileSystem::Listener {
		private:
			class LuaProcessor* parent = nullptr;
		public:
			LuaFileSystemListener(class LuaProcessor* parent) : parent(parent) {}
			
			virtual void onUnmounted(FileSystem::Path path, FileSystem::SRef<FileSystem::Device> device) override;
			virtual void onNodeRemoved(FileSystem::Path path, FileSystem::NodeType type) override;
		};

		enum LuaTickState {
			LUA_SYNC		= 0b00001,
			LUA_ASYNC		= 0b00010,
			LUA_ERROR		= 0b00100,
			LUA_END			= 0b01000,
			LUA_BEGIN		= 0b10000,
			LUA_SYNC_ERROR	= LUA_SYNC | LUA_ERROR,
			LUA_SYNC_END	= LUA_SYNC | LUA_END,
			LUA_ASYNC_BEGIN	= LUA_ASYNC | LUA_BEGIN,
			LUA_ASYNC_ERROR	= LUA_ASYNC | LUA_ERROR,
			LUA_ASYNC_END	= LUA_ASYNC | LUA_END,
		};
		ENUM_CLASS_FLAGS(LuaTickState);

		struct FLuaTickRunnable : public FNonAbandonableTask {
		private:
			class LuaProcessorTick* Tick;
			
		public:	
			FLuaTickRunnable(class LuaProcessorTick* Tick) : Tick(Tick) {}
			
			void DoWork();

			FORCEINLINE TStatId GetStatId() const {
				RETURN_QUICK_DECLARE_CYCLE_STAT(ExampleAsyncTask, STATGROUP_ThreadPoolAsyncTasks);
			}
		};

		class LuaProcessorTick {
			// Lua Tick state lua step lenghts
			int SyncLen = 2500;
			int SyncErrorLen = 1250;
			int SyncEndLen = 500;
			int AsyncLen = 2500;
			int AsyncErrorLen = 1200;
			int AsyncEndLen = 500;
			
		private:
			class LuaProcessor* Processor;
			LuaTickState State = LUA_SYNC;
			FLuaTickRunnable Runnable;
			TSharedPtr<FAsyncTask<FLuaTickRunnable>> asyncTask;
			FCriticalSection StateMutex;
			FCriticalSection TickMutex;
			FCriticalSection AsyncSyncMutex;
			bool bShouldPromote = false;
			bool bShouldDemote = false;
			bool bShouldStop = false;
			bool bShouldReset = false;
			bool bShouldCrash = false;
			bool bDoSync = false;
			KernelCrash ToCrash;
			TPromise<void> AsyncSync;
			TPromise<void> SyncAsync;
			
		public:
			LuaProcessorTick(class LuaProcessor* Processor);

			~LuaProcessorTick();

			void reset();
			void stop();
			void promote();
			void demote();
			void demoteInAsync();
			void shouldStop();
			void shouldReset();
			void shouldPromote();
			void shouldDemote();
			void shouldCrash(const KernelCrash& Crash);
			int steps();
			
			void syncTick();
			bool asyncTick();
			bool postTick();

			void tickHook(lua_State* L);
			int apiReturn(lua_State* L, int args);
		};
		
		class LuaProcessor : public Processor {
			friend int luaPull(lua_State* L);
			friend int luaComputerSkip(lua_State* L);
			friend FLuaTickRunnable;
			friend struct FLuaSyncCall;
			friend LuaProcessorTick;

		private:

			// Processor cache
			TWeakObjectPtr<AFINStateEEPROMLua> eeprom;

			// Lua runtime
			lua_State* luaState = nullptr;
			lua_State* luaThread = nullptr;
			int luaThreadIndex = 0;
			LuaProcessorTick tickHelper;

			// signal pulling
			int pullState = 0; // 0 = not pulling, 1 = pulling with timeout, 2 = pull indefinetly
			double timeout = 0.0;
			std::chrono::time_point<std::chrono::high_resolution_clock> pullStart;

			// filesystem handling
			std::set<LuaFile> fileStreams;
			FileSystem::SRef<LuaFileSystemListener> fileSystemListener;
			
		public:
			static LuaProcessor* luaGetProcessor(lua_State* L);
			
			LuaProcessor(int speed = 1);
			~LuaProcessor();

			// Begin Processor
			virtual void setKernel(KernelSystem* kernel) override;
			virtual void tick(float delta) override;
			virtual void stop(bool isCrash) override;
			virtual void reset() override;
			virtual std::int64_t getMemoryUsage(bool recalc = false) override;
			virtual void PreSerialize(UProcessorStateStorage* Storage, bool bLoading) override;
			virtual void Serialize(UProcessorStateStorage* Storage, bool bLoading) override;
			virtual void PostSerialize(UProcessorStateStorage* Storage, bool bLoading) override;
			virtual UProcessorStateStorage* CreateSerializationStorage() override;
			virtual void setEEPROM(AFINStateEEPROM* eeprom) override;
			// End Processor

			/**
			 * returns the tick helper
			 */
			LuaProcessorTick& getTickHelper();
			
			/**
			 * Executes one lua tick sync or async.
			 * Might set after tick cache which should get handled by tick.
			 */
			void luaTick();

			/**
			* Sets up the lua environment.
			* Adds the Computer API in global namespace and adds the FileSystem API in global namespace.
			*
			* @param[in]	L	the lua context you want to setup
			*/
			void luaSetup(lua_State* L);

			/**
			 * Allows to access the the eeprom used by the processor.
			 * Nullptr if no eeprom is currently set.
			 *
			 * @return	the eeprom used by the processor.
			 */
			AFINStateEEPROMLua* getEEPROM();

			/**
			 * Trys to pop a signal from the signal queue in the network controller
			 * and pushes the resulting values to the given lua stack.
			 *
			 * @param[in]	L	the stack were the values should get pushed to.
			 * @return	the count of values we have pushed.
			 */
			int doSignal(lua_State* L);
			
			void clearFileStreams();
			std::set<LuaFile> getFileStreams() const;

			static void luaHook(lua_State* L, lua_Debug* ar);

			/**
			 * Access the lua processor in the given state registry and checks
			 * if the tick process is in the second stage of execution.
			 * If this is the case yields the runtime with a continuation function
			 * which will return values of the count of args on top of the stack prior
			 * to the yield.
			 * If the tick is in the first stage, we just return.
			 *
			 * @param[in]	L		the lua state all of this should occur
			 * @param[in]	args	the count of arguments we should copy and the continuation should return
			 * @return	if it even returns, returns the same as args
			 */
			static int luaAPIReturn(lua_State* L, int args);

			/**
			 * Returns the lua state
			 */
			lua_State* getLuaState() const;
		};

		struct FLuaSyncCall {
			LuaProcessor* Processor;

			FLuaSyncCall(lua_State* L) {
				Processor = LuaProcessor::luaGetProcessor(L);
				Processor->tickHelper.demoteInAsync();
			}
        };
	}
}
