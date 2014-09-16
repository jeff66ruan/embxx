//
// Copyright 2013 (C). Alex Robenko. All rights reserved.
//

// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.


#pragma once

#include <functional>
#include <algorithm>
#include <vector>
#include <memory>
#include <list>
#include <cstdint>
#include <map>
#include <mutex>
#include <cassert>

#include "cxxtest/TestSuite.h"

#include "embxx/error/ErrorStatus.h"
#include "embxx/device/context.h"

#include "ReadFifo.h"
#include "WriteFifo.h"
#include "TestDevice.h"

namespace embxx
{

namespace device
{

namespace test
{

template <typename TEventLoopLock,
          typename TCharType = std::uint8_t,
          std::size_t TFifoSize = 16,
          std::size_t TFifoOpDelayMs = 2>
class SpiDevice : public TestDevice
{
    typedef TestDevice Base;

public:

    // Required definitions
    typedef unsigned DeviceIdType;
    typedef TCharType CharType;

    // Implementation specific definitions
    typedef TEventLoopLock EventLoopLock;
    typedef ReadFifo<CharType, TFifoSize, TFifoOpDelayMs> RFifo;
    typedef WriteFifo<CharType, TFifoSize, TFifoOpDelayMs> WFifo;

    typedef typename RFifo::DataSeq ReadDataSeq;
    typedef typename WFifo::DataSeq WriteDataSeq;

    typedef std::function<void ()> CanDoOpHandler;
    typedef std::function<void (const embxx::error::ErrorStatus&)> OpCompleteHandler;

    // Creation and configuration interface

    SpiDevice(EventLoopLock& lock)
        : Base(),
          lock_(lock),
          currDevId_(0),
          remainingReadLen_(0),
          remainingWriteLen_(0),
          readFifo_(io_),
          writeFifo_(io_),
          suspended_(false)
    {
        readFifo_.setReadAvailableHandler(
            std::bind(
                &SpiDevice::readAvailableHandler,
                this));

        writeFifo_.setWriteAvailableHandler(
            std::bind(
                &SpiDevice::writeAvailableHandler,
                this));
    }

    ~SpiDevice()
    {
        {
            std::lock_guard<EventLoopLock> guard(lock_);
            readFifo_.clear();
            writeFifo_.clear();
            readFifo_.setReadAvailableHandler(nullptr);
            writeFifo_.setWriteAvailableHandler(nullptr);
            suspended_ = false;
            suspendCond_.notify_all();
        }
        stopThread();
    }

    void setDataToRead(DeviceIdType id, const CharType* data, std::size_t size)
    {
        std::lock_guard<EventLoopLock> guard(lock_);
        auto& seq = dataToRead_[id];
        assert(seq.empty());
        seq.assign(data, data + size);
    }

    const WriteDataSeq& getWrittenData(DeviceIdType id) const
    {
        std::lock_guard<EventLoopLock> guard(lock_);
        auto iter = writtenData_.find(id);
        assert(iter != writtenData_.end());
        return iter->second;
    }

    void clearWrittenData(DeviceIdType id)
    {
        std::lock_guard<EventLoopLock> guard(lock_);
        writtenData_[id].clear();
    }

    // Required interface
    template <typename TFunc>
    void setCanReadHandler(TFunc&& func)
    {
        std::lock_guard<EventLoopLock> guard(lock_);
        canReadHandler_ = std::forward<TFunc>(func);
    }

    template <typename TFunc>
    void setCanWriteHandler(TFunc&& func)
    {
        std::lock_guard<EventLoopLock> guard(lock_);
        canWriteHandler_ = std::forward<TFunc>(func);
    }

    template <typename TFunc>
    void setReadCompleteHandler(TFunc&& func)
    {
        std::lock_guard<EventLoopLock> guard(lock_);
        readCompleteHandler_ = std::forward<TFunc>(func);
    }

    template <typename TFunc>
    void setWriteCompleteHandler(TFunc&& func)
    {
        std::lock_guard<EventLoopLock> guard(lock_);
        writeCompleteHandler_ = std::forward<TFunc>(func);
    }

    void startRead(
        DeviceIdType id,
        std::size_t length,
        embxx::device::context::EventLoop context)
    {
        static_cast<void>(context);
        std::lock_guard<EventLoopLock> guard(lock_);
        startReadInternal(id, length);
    }

    void startRead(
        DeviceIdType id,
        std::size_t length,
        embxx::device::context::Interrupt context)
    {
        static_cast<void>(context);
        startReadInternal(id, length);
    }

    bool cancelRead(
        embxx::device::context::EventLoop context)
    {
        static_cast<void>(context);
        std::lock_guard<EventLoopLock> guard(lock_);
        return cancelReadInternal();
    }

    bool cancelRead(
        embxx::device::context::Interrupt context)
    {
        static_cast<void>(context);
        return cancelReadInternal();
    }

    void startWrite(
        DeviceIdType id,
        std::size_t length,
        embxx::device::context::EventLoop context)
    {
        static_cast<void>(context);
        std::lock_guard<EventLoopLock> guard(lock_);
        startWriteInternal(id, length);
    }

    void startWrite(
        DeviceIdType id,
        std::size_t length,
        embxx::device::context::Interrupt context)
    {
        static_cast<void>(context);
        startWriteInternal(id, length);
    }

    bool cancelWrite(
        embxx::device::context::EventLoop context)
    {
        static_cast<void>(context);
        std::lock_guard<EventLoopLock> guard(lock_);
        return cancelWriteInternal();
    }

    bool cancelWrite(
        embxx::device::context::Interrupt context)
    {
        static_cast<void>(context);
        return cancelWriteInternal();
    }

    bool suspend(
        embxx::device::context::EventLoop context)
    {
        static_cast<void>(context);
        std::lock_guard<EventLoopLock> guard(lock_);

        if ((remainingReadLen_ == 0) && (remainingWriteLen_ == 0)) {
            return false;
        }

        assert(!suspended_);

        suspended_ = true;
        return true;
    }

    void resume(
        embxx::device::context::EventLoop context)
    {
        static_cast<void>(context);
        std::lock_guard<EventLoopLock> guard(lock_);
        assert(suspended_);
        suspended_ = false;
        suspendCond_.notify_all();
    }

    bool canRead(embxx::device::context::Interrupt context)
    {
        static_cast<void>(context);
        return (readFifo_.canRead() && (0 < remainingReadLen_));
    }

    bool canWrite(embxx::device::context::Interrupt context)
    {
        static_cast<void>(context);
        return (writeFifo_.canWrite() && (0 < remainingWriteLen_));
    }

    CharType read(embxx::device::context::Interrupt context)
    {
        static_cast<void>(context);
        assert(canRead(context));
        --remainingReadLen_;
        return readFifo_.read();
    }

    void write(
        CharType value,
        embxx::device::context::Interrupt context)
    {
        static_cast<void>(context);
        assert(canWrite(context));
        writeFifo_.write(value);
        --remainingWriteLen_;
    }

private:

    typedef std::map<DeviceIdType, ReadDataSeq> ReadDataSeqMap;
    typedef std::map<DeviceIdType, WriteDataSeq> WriteDataSeqMap;

    void startReadInternal(
        DeviceIdType id,
        std::size_t length)
    {
        assert(remainingReadLen_ == 0);
        assert((remainingWriteLen_ == 0) || (currDevId_ == id));
        assert(!dataToRead_[id].empty());
        assert(canReadHandler_);
        assert(readCompleteHandler_);
        assert(length <= dataToRead_[id].size());
        currDevId_ = id;
        remainingReadLen_ = length;
        ReadDataSeq dataToRead;
        dataToRead.assign(dataToRead_[id].begin(), dataToRead_[id].begin() + length);
        readFifo_.setDataToRead(std::move(dataToRead));
        readFifo_.startRead();
    }

    bool cancelReadInternal()
    {
        if (remainingReadLen_ == 0) {
            return false;
        }

        finaliseRead();
        return true;
    }

    void readAvailableHandler()
    {
        std::unique_lock<EventLoopLock> guard(lock_);
        suspendCond_.wait(guard,
            [this]() -> bool
            {
                return (!suspended_);
            });

        assert(readFifo_.canRead());
        if (canRead(embxx::device::context::Interrupt())) {
            assert(canReadHandler_);
            canReadHandler_();
        }

        if (remainingReadLen_ == 0) {
            assert(readFifo_.complete());
            finaliseRead();
            assert(readCompleteHandler_);
            readCompleteHandler_(embxx::error::ErrorCode::Success);
        }
    }

    void finaliseRead()
    {
        auto& data = dataToRead_[currDevId_];
        auto& remData = readFifo_.getDataToRead();
        data.insert(data.begin(), remData.begin(), remData.end());
        readFifo_.clear();
        remainingReadLen_ = 0;
    }

    void startWriteInternal(
        DeviceIdType id,
        std::size_t length)
    {
        assert(0 < length);
        assert(remainingWriteLen_ == 0);
        assert((remainingReadLen_ == 0) || (currDevId_ == id));
        assert(canWriteHandler_);
        assert(writeCompleteHandler_);
        currDevId_ = id;
        remainingWriteLen_ = length;
        assert(writeFifo_.empty());
        writeFifo_.startWrite();
    }

    bool cancelWriteInternal()
    {
        if (remainingWriteLen_ == 0) {
            return false;
        }

        finaliseWrite();
        return true;
    }

    void writeAvailableHandler()
    {
        std::unique_lock<EventLoopLock> guard(lock_);
        suspendCond_.wait(guard,
            [this]() -> bool
            {
                return (!suspended_);
            });

        assert(canWriteHandler_);
        if (canWrite(embxx::device::context::Interrupt())) {
            canWriteHandler_();
        }

        if ((remainingWriteLen_ == 0) && (writeFifo_.complete())) {
            finaliseWrite();
            assert(writeCompleteHandler_);
            writeCompleteHandler_(embxx::error::ErrorCode::Success);
        }
    }

    void finaliseWrite()
    {
        auto& data = writtenData_[currDevId_];
        auto& writtenData = writeFifo_.getWrittenData();
        data.insert(data.end(), writtenData.begin(), writtenData.end());
        writeFifo_.clear();
        remainingWriteLen_ = 0;
    }

    EventLoopLock& lock_;
    DeviceIdType currDevId_;
    volatile std::size_t remainingReadLen_;
    volatile std::size_t remainingWriteLen_;
    ReadDataSeqMap dataToRead_;
    WriteDataSeqMap writtenData_;
    RFifo readFifo_;
    WFifo writeFifo_;
    CanDoOpHandler canReadHandler_;
    CanDoOpHandler canWriteHandler_;
    OpCompleteHandler readCompleteHandler_;
    OpCompleteHandler writeCompleteHandler_;
    volatile bool suspended_;

    std::condition_variable_any suspendCond_;
};

}  // namespace test

}  // namespace device

}  // namespace embxx
