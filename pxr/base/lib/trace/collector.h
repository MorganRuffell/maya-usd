//
// Copyright 2018 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//

#ifndef TRACE_COLLECTOR_H
#define TRACE_COLLECTOR_H

#include "pxr/pxr.h"

#include "pxr/base/trace/api.h"
#include "pxr/base/trace/concurrentList.h"
#include "pxr/base/trace/collection.h"
#include "pxr/base/trace/event.h"
#include "pxr/base/trace/key.h"
#include "pxr/base/trace/threads.h"

#include "pxr/base/tf/declarePtrs.h"
#include "pxr/base/tf/mallocTag.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED
#include "pxr/base/tf/pyTracing.h"
#endif // PXR_PYTHON_SUPPORT_ENABLED

#include "pxr/base/tf/singleton.h"
#include "pxr/base/tf/refBase.h"
#include "pxr/base/tf/refPtr.h"
#include "pxr/base/tf/weakBase.h"
#include "pxr/base/tf/weakPtr.h"

#include <atomic>
#include <string>
#include <vector>

#include <tbb/spin_mutex.h>

PXR_NAMESPACE_OPEN_SCOPE

class TraceScopeHolder;
 
TF_DECLARE_WEAK_PTRS(TraceCollector);
TF_DECLARE_WEAK_AND_REF_PTRS(TraceScope);


////////////////////////////////////////////////////////////////////////////////
/// \class TraceCollector
///
/// This is a singleton class that records TraceEvent instances and populates 
/// TraceCollection instances.
///
/// All public methods of TraceCollector are safe to call from any thread.
class TraceCollector : public TfWeakBase {
public:

    TF_MALLOC_TAG_NEW("Trace", "TraceCollector");

    using This = TraceCollector;
    using ThisPtr = TraceCollectorPtr;

    using TimeStamp = TraceEvent::TimeStamp;

    using Key = TraceDynamicKey;

    /// Returns the singleton instance.
    TRACE_API static TraceCollector& GetInstance() {
         return TfSingleton<TraceCollector>::GetInstance();
    }
    
    TRACE_API ~TraceCollector();

    ///  Enables or disables collection of events.
    TRACE_API void SetEnabled(bool isEnabled);

    ///  Returns whether collection of events is enabled.
    static bool IsEnabled() {
        return (_isEnabled.load(std::memory_order_acquire) == 1);
    }

#ifdef PXR_PYTHON_SUPPORT_ENABLED
    /// Returns whether automatic tracing of all python scopes is enabled.
    bool IsPythonTracingEnabled() const {
        return _isPythonTracingEnabled.load(std::memory_order_acquire) != 0;
    }

    /// Set whether automatic tracing of all python scopes is enabled.
    TRACE_API void SetPythonTracingEnabled(bool enabled);
#endif // PXR_PYTHON_SUPPORT_ENABLED
    
    /// Clear all pending events from the collector. No TraceCollection will be 
    /// made for these events.
    TRACE_API void Clear();

    /// \name Event Recording
    /// @{

    /// Record a begin event with \a key if collection of events is enabled.
    /// A matching end event is expected some time in the future.
    ///
    /// If the key is known at compile time \c BeginScope and \c Scope methods 
    /// are preferred because they have lower overhead.
    /// \returns The TimeStamp of the TraceEvent or 0 if the collector is 
    /// disabled.
    /// \sa BeginScope \sa Scope
    TRACE_API TimeStamp BeginEvent(
        const Key& key, TraceCategoryId cat = TraceCategory::Default);

    /// Record a begin event with \a key at a specified time if collection of 
    /// events is enabled.
    /// This version of the method allows the passing of a specific number of
    /// elapsed milliseconds, \a ms, to use for this event.
    /// This method is used for testing and debugging code.
    TRACE_API void BeginEventAtTime(const Key& key, 
                          double ms, 
                          TraceCategoryId cat = TraceCategory::Default);

    /// Record an end event with \a key if collection of events is enabled.
    /// A matching begin event must have preceded this end event.
    ///
    /// If the key is known at compile time EndScope and Scope methods are
    /// preferred because they have lower overhead.
    /// \returns The TimeStamp of the TraceEvent or 0 if the collector is 
    /// disabled.
    /// \sa EndScope \sa Scope
    TRACE_API TimeStamp EndEvent(const Key& key, 
                       TraceCategoryId cat = TraceCategory::Default);

    /// Record an end event with \a key at a specified time if collection of 
    /// events is enabled.
    /// This version of the method allows the passing of a specific number of
    /// elapsed milliseconds, \a ms, to use for this event.
    /// This method is used for testing and debugging code.
    TRACE_API void EndEventAtTime(const Key& key, 
                        double ms, 
                        TraceCategoryId cat = TraceCategory::Default);

    /// Record a begin event for a scope described by \a key if collection of 
    /// events is enabled.
    /// It is more efficient to use the \c Scope method than to call both
    /// \c BeginScope and \c EndScope.
    /// \sa EndScope \sa Scope
    void BeginScope(
        const TraceKey& _key, TraceCategoryId cat = TraceCategory::Default) {
        if (ARCH_LIKELY(!IsEnabled()))
            return;

        _BeginScope(_key, cat);
    }

    /// Record a begin event for a scope described by \a key and a specified
    /// category and store data arguments if collection of events is enabled.
    /// The variadic arguments \a args must be an even number of parameters in 
    /// the form TraceKey, Value.
    /// \sa EndScope \sa Scope \sa StoreData
    template <typename... Args>
    void BeginScope(
        const TraceKey& key, TraceCategoryId cat, Args&&... args) {
        static_assert( sizeof...(Args) %2 == 0, 
            "Data arguments must come in pairs");

        if (ARCH_LIKELY(!IsEnabled()))
            return;

        _PerThreadData *threadData = _GetThreadData();
        threadData->BeginScope(key, cat);
        _StoreDataRec(threadData, cat, std::forward<Args>(args)...);
    }

    /// Record a begin event for a scope described by \a key and store data 
    /// arguments if collection of events is enabled. The variadic arguments 
    /// \a args must be an even number of parameters in the form TraceKey, Value.
    /// \sa EndScope \sa Scope \sa StoreData
    template <typename... Args>
    void BeginScope(const TraceKey& key, Args&&... args) {
        static_assert( sizeof...(Args) %2 == 0, 
            "Data arguments must come in pairs");

        // Explcicitly cast to TraceCategoryId so overload resolution choose the
        // version with a category arguement.
        BeginScope(key, 
            static_cast<TraceCategoryId>(TraceCategory::Default),
            std::forward<Args>(args)...);
    }

    /// Record an end event described by  \a key if collection of events is 
    /// enabled.
    /// It is more efficient to use the \c Scope method than to call both
    /// \c BeginScope and \c EndScope.
    /// \sa BeginScope \sa Scope
    void EndScope(
        const TraceKey& key, TraceCategoryId cat = TraceCategory::Default) {
        if (ARCH_LIKELY(!IsEnabled()))
            return;

        _EndScope(key, cat);
    }

    /// Record a scope event described by \a key that started at \a start if
    /// collection of events is enabled.
    ///
    /// This method is used by the TRACE_FUNCTION, TRACE_SCOPE and
    /// TRACE_FUNCTION_SCOPE macros.
    /// \sa BeginScope \sa EndScope
    void Scope(const TraceKey& key, TimeStamp start,
        TraceCategoryId cat = TraceCategory::Default) {
        if (ARCH_LIKELY(!IsEnabled()))
            return;

        _PerThreadData *threadData = _GetThreadData();
        threadData->EmplaceEvent(TraceEvent::Timespan, key,  start, cat);
    }

    /// Record multiple data events with category \a cat if collection of events
    /// is enabled.
    /// \sa StoreData
    template <typename... Args>
    void ScopeArgs(TraceCategoryId cat, Args&&... args) {
        static_assert( sizeof...(Args) %2 == 0, 
            "Data arguments must come in pairs");

        if (ARCH_LIKELY(!IsEnabled()))
            return;

        _PerThreadData *threadData = _GetThreadData();
        _StoreDataRec(threadData, cat, std::forward<Args>(args)...);
    }

    /// Record multiple data events with the default category if collection of 
    /// events is enabled.
    /// The variadic arguments \a args must be an even number of parameters in 
    /// the form TraceKey, Value. It is more efficient to use this method to 
    /// store multiple data items than to use multiple calls to \c StoreData.
    /// \sa StoreData
    template <typename... Args>
    void ScopeArgs(Args&&... args) {
        static_assert( sizeof...(Args) %2 == 0, 
            "Data arguments must come in pairs");

        ScopeArgs(static_cast<TraceCategoryId>(TraceCategory::Default),
            std::forward<Args>(args)...);
    }

    /// Record a data event with the given \a key and \a value if collection of 
    /// events is enabled. \a value may be  of any type which a TraceEvent can
    /// be constructed from (bool, int, std::string, uint64, double).
    /// \sa ScopeArgs
    template <typename T>
    void StoreData(
            const TraceKey &key, const T& value, 
            TraceCategoryId cat = TraceCategory::Default) {
        if (ARCH_UNLIKELY(IsEnabled())) {
            _StoreData(_GetThreadData(), key, cat, value);
        }
    }

    /// Record a counter \a delta for a name \a key if collection of events is
    /// enabled.
    void RecordCounterDelta(const TraceKey &key, 
                            double delta, 
                            TraceCategoryId cat = TraceCategory::Default) {
        // Only record counter values if the collector is enabled.
        if (ARCH_UNLIKELY(IsEnabled())) {
            _PerThreadData *threadData = _GetThreadData();
            threadData->EmplaceEvent(
                TraceEvent::CounterDelta, key, delta, cat);
        }
    }

    /// Record a counter \a delta for a name \a key  if collection of events is 
    /// enabled.
    void RecordCounterDelta(const Key &key, 
                            double delta, 
                            TraceCategoryId cat = TraceCategory::Default) {

        if (ARCH_UNLIKELY(IsEnabled())) {
            _PerThreadData *threadData = _GetThreadData();
            threadData->CounterDelta(key, delta, cat);
        }
    }

    /// Record a counter \a value for a name \a key if collection of events is
    /// enabled.
    void RecordCounterValue(const TraceKey &key, 
                            double value, 
                            TraceCategoryId cat = TraceCategory::Default) {
        // Only record counter values if the collector is enabled.
        if (ARCH_UNLIKELY(IsEnabled())) {
            _PerThreadData *threadData = _GetThreadData();
            threadData->EmplaceEvent(
                TraceEvent::CounterValue, key, value, cat);
        }
    }

    /// Record a counter \a value for a name \a key and delta \a value  if
    /// collection of events is enabled.
    void RecordCounterValue(const Key &key, 
                            double value, 
                            TraceCategoryId cat = TraceCategory::Default) {

        if (ARCH_UNLIKELY(IsEnabled())) {
            _PerThreadData *threadData = _GetThreadData();
            threadData->CounterValue(key, value, cat);
        }
    }

    /// @}

    ///  Return the label associated with this collector.
    const std::string& GetLabel() {
        return _label;
    }

    /// Produces a TraceCollection from all the events that recorded in the 
    /// collector and issues a TraceCollectionAvailable notice. Note that
    /// creating a collection restarts tracing, i.e. events contained in this
    /// collection will not be present in subsequent collections.
    TRACE_API void CreateCollection();

private:

    TraceCollector();

    friend class TfSingleton<TraceCollector>;

    class _PerThreadData;

    // Return a pointer to existing per-thread data or create one if none
    // exists.
    TRACE_API _PerThreadData* _GetThreadData();

    // This is the fast execution path called from the TRACE_FUNCTION
    // and TRACE_SCOPE macros
    void _BeginScope(const TraceKey& key, TraceCategoryId cat)
    {
        // Note we're not calling _NewEvent, don't need to cache key
        _PerThreadData *threadData = _GetThreadData();
        threadData->BeginScope(key, cat);
    }

    // This is the fast execution path called from the TRACE_FUNCTION
    // and TRACE_SCOPE macros    
    TRACE_API void _EndScope(const TraceKey& key, TraceCategoryId cat);

#ifdef PXR_PYTHON_SUPPORT_ENABLED
    // Callback function registered as a python tracing function.
    void _PyTracingCallback(const TfPyTraceInfo &info);
#endif // PXR_PYTHON_SUPPORT_ENABLED

    // Implementation for small data that can stored inlined with the event.
    template <typename T,
        typename std::enable_if<
            sizeof(T) <= sizeof(uintptr_t) 
            && !std::is_pointer<T>::value , int>::type = 0>
    void _StoreData(_PerThreadData* threadData, const TraceKey &key,
        TraceCategoryId cat, const T& value) {
        threadData->StoreData(key, value, cat);
    }

    // Implementation for data that must be stored outside of the events.
    template <typename T,
        typename std::enable_if<
            (sizeof(T) > sizeof(uintptr_t))
            && !std::is_pointer<T>::value, int>::type = 0>
    void _StoreData(_PerThreadData* threadData, const TraceKey &key,
        TraceCategoryId cat,  const T& value) {
        threadData->StoreLargeData(key, value, cat);
    }

    // Specialization for c string
    void _StoreData(
        _PerThreadData* threadData, 
        const TraceKey &key, 
        TraceCategoryId cat, 
        const char* value) {
        threadData->StoreLargeData(key, value, cat);
    }

    // Specialization for std::string
    void _StoreData(
        _PerThreadData* threadData,
        const TraceKey &key,
        TraceCategoryId cat, 
        const std::string& value) {
        threadData->StoreLargeData(key, value.c_str(), cat);
    }

    // Variadic version to store multiple data events in one function call.
    template <typename K, typename T, typename... Args>
    void _StoreDataRec(
        _PerThreadData* threadData, TraceCategoryId cat, K&& key, 
        const T& value, Args&&... args) {
        _StoreData(threadData, std::forward<K>(key), cat, value);
        _StoreDataRec(threadData, cat, std::forward<Args>(args)...);
    }

    // Base case to terminate template recursion
    void _StoreDataRec(_PerThreadData* threadData, TraceCategoryId cat) {}


    // Thread-local storage, accessed via _GetThreadData()
    //
    class _PerThreadData {
        public:
            using EventList = TraceCollection::EventList;

            _PerThreadData();
            ~_PerThreadData();

            const TraceThreadId& GetThreadId() const {
                return _threadIndex;
            }
            TimeStamp BeginEvent(const Key& key, TraceCategoryId cat);
            TimeStamp EndEvent(const Key& key, TraceCategoryId cat);

            // Debug Methods
            void BeginEventAtTime(
                const Key& key, double ms, TraceCategoryId cat);
            void EndEventAtTime(const Key& key, double ms, TraceCategoryId cat);

            void BeginScope(const TraceKey& key, TraceCategoryId cat) {
                AtomicRef lock(_writing);
                _BeginScope(key, cat);
            }

            void EndScope(const TraceKey& key, TraceCategoryId cat) {
                AtomicRef lock(_writing);
                _EndScope(key, cat);
            }

            TRACE_API void CounterDelta(
                const Key&, double value, TraceCategoryId cat);

            TRACE_API void CounterValue(
                const Key&, double value, TraceCategoryId cat);

            template <typename T>
            void StoreData(
                const TraceKey& key, const T& data, TraceCategoryId cat) {
                AtomicRef lock(_writing);
                _events.load(std::memory_order_acquire)->EmplaceBack(
                    TraceEvent::Data, key, data, cat);
            }

            template <typename T>
            void StoreLargeData(
                const TraceKey& key, const T& data, TraceCategoryId cat) {
                AtomicRef lock(_writing);
                EventList* events = _events.load(std::memory_order_acquire);
                const auto* cached = events->StoreData(data);
                events->EmplaceBack(TraceEvent::Data, key, cached, cat);
            }

            template <typename... Args>
            void EmplaceEvent(Args&&... args) {
                AtomicRef lock(_writing);
                _events.load(std::memory_order_acquire)->EmplaceBack(
                    std::forward<Args>(args)...);
            }

#ifdef PXR_PYTHON_SUPPORT_ENABLED
            void PushPyScope(const Key& key, bool enabled);
            void PopPyScope(bool enabled);
#endif // PXR_PYTHON_SUPPORT_ENABLED

            // These methods can be called from threads at the same time as the 
            // other methods.
            std::unique_ptr<EventList> GetCollectionData();
            void Clear();

        private:
            void _BeginScope(const TraceKey& key, TraceCategoryId cat) {
                _events.load(std::memory_order_acquire)->EmplaceBack(
                    TraceEvent::Begin, key, cat);
            }

            void _EndScope(const TraceKey& key, TraceCategoryId cat);

            // Flag to let other threads know that the list is being written to.
            mutable std::atomic<bool> _writing;
            std::atomic<EventList*> _events;

            class AtomicRef {
            public:
                AtomicRef(std::atomic<bool>& b) : _bool(b) {
                    _bool.store(true, std::memory_order_release);
                }
                ~AtomicRef() {
                    _bool.store(false, std::memory_order_release);
                }
            private:
                std::atomic<bool>& _bool;
            };

            // An integer that is unique for each thread launched by any
            // threadDispatcher.  Each time a thread is Start-ed it get's
            // a new id.
            //
            TraceThreadId _threadIndex;

#ifdef PXR_PYTHON_SUPPORT_ENABLED
            // When auto-tracing python frames, this stores the stack of scopes.
            struct PyScope {
                Key key;
            };
            std::vector<PyScope> _pyScopes;
#endif // PXR_PYTHON_SUPPORT_ENABLED
    };

    TRACE_API static std::atomic<int> _isEnabled;

    // A list with one _PerThreadData per thread.
    TraceConcurrentList<_PerThreadData> _allPerThreadData;

    std::string _label;

#ifdef PXR_PYTHON_SUPPORT_ENABLED
    std::atomic<int> _isPythonTracingEnabled;
    TfPyTraceFnId _pyTraceFnId;
#endif // PXR_PYTHON_SUPPORT_ENABLED
};
 
TRACE_API_TEMPLATE_CLASS(TfSingleton<TraceCollector>);

PXR_NAMESPACE_CLOSE_SCOPE

#endif // TRACE_COLLECTOR_H