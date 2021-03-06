/*
 * Copyright 2014 - 2015 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDED_AERON_CONCURRENT_LOGBUFFER_TERM_APPENDER__
#define INCLUDED_AERON_CONCURRENT_LOGBUFFER_TERM_APPENDER__

#include <util/Index.h>
#include <concurrent/AtomicBuffer.h>
#include "LogBufferDescriptor.h"
#include "LogBufferPartition.h"
#include "BufferClaim.h"
#include "DataFrameHeader.h"

namespace aeron { namespace concurrent { namespace logbuffer {

#define TERM_APPENDER_TRIPPED ((std::int32_t)-1)
#define TERM_APPENDER_FAILED ((std::int32_t)-2)

class TermAppender : public LogBufferPartition
{
public:
    TermAppender(
        AtomicBuffer& termBuffer, AtomicBuffer& metaDataBuffer, AtomicBuffer& defaultHdr, util::index_t maxFrameLength) :
        LogBufferPartition(termBuffer, metaDataBuffer),
        m_defaultHdr(defaultHdr),
        m_maxMessageLength(FrameDescriptor::computeMaxMessageLength(termBuffer.capacity())),
        m_maxFrameLength(maxFrameLength),
        m_maxPayloadLength(m_maxFrameLength - DataFrameHeader::LENGTH),
        m_defaultHdrSessionId(defaultHdr.getInt32(DataFrameHeader::SESSION_ID_FIELD_OFFSET)),
        m_defaultHdrStreamId(defaultHdr.getInt32(DataFrameHeader::STREAM_ID_FIELD_OFFSET))
    {
        FrameDescriptor::checkHeaderLength(defaultHdr.capacity());
        FrameDescriptor::checkMaxFrameLength(maxFrameLength);
    }

    inline util::index_t maxMessageLength()
    {
        return m_maxMessageLength;
    }

    inline util::index_t maxPayloadLength()
    {
        return m_maxPayloadLength;
    }

    inline util::index_t maxFrameLength()
    {
        return m_maxFrameLength;
    }

    inline std::int32_t append(AtomicBuffer& srcBuffer, util::index_t offset, util::index_t length)
    {
        std::int32_t resultingOffset;

        if (length <= m_maxPayloadLength)
        {
            resultingOffset = appendUnfragmentedMessage(srcBuffer, offset, length);
        }
        else
        {
            if (length > m_maxMessageLength)
            {
                throw util::IllegalArgumentException(
                    util::strPrintf(
                        "encoded message exceeds maxMessageLength of %d, length=%d", m_maxMessageLength, length), SOURCEINFO);
            }

            resultingOffset = appendFragmentedMessage(srcBuffer, offset, length);
        }

        return resultingOffset;
    }

    inline std::int32_t claim(util::index_t length, BufferClaim& bufferClaim)
    {
        if (length > m_maxPayloadLength)
        {
            throw util::IllegalArgumentException(
                util::strPrintf("claim exceeds maxPayloadLength of %d, length=%d", m_maxPayloadLength, length), SOURCEINFO);
        }

        const util::index_t frameLength = length + DataFrameHeader::LENGTH;
        const util::index_t alignedLength = util::BitUtil::align(frameLength, FrameDescriptor::FRAME_ALIGNMENT);
        const util::index_t frameOffset = metaDataBuffer().getAndAddInt32(
            LogBufferDescriptor::TERM_TAIL_COUNTER_OFFSET, alignedLength);
        AtomicBuffer& buffer = termBuffer();
        const std::int32_t capacity = buffer.capacity();

        std::int32_t resultingOffset = frameOffset + alignedLength;
        if (resultingOffset > (capacity - DataFrameHeader::LENGTH))
        {
            resultingOffset = handleEndOfLogCondition(buffer, frameOffset, capacity);
        }
        else
        {
            applyDefaultHeader(buffer, frameOffset, frameLength);
            bufferClaim.wrap(buffer, frameOffset, frameLength);
        }

        return resultingOffset;
    }

private:
    AtomicBuffer m_defaultHdr;
    const util::index_t m_maxMessageLength;
    const util::index_t m_maxFrameLength;
    const util::index_t m_maxPayloadLength;
    const std::int32_t m_defaultHdrSessionId;
    const std::int32_t m_defaultHdrStreamId;

    std::int32_t appendUnfragmentedMessage(AtomicBuffer& srcBuffer, util::index_t srcOffset, util::index_t length)
    {
        const util::index_t frameLength = length + DataFrameHeader::LENGTH;
        const util::index_t alignedLength = util::BitUtil::align(frameLength, FrameDescriptor::FRAME_ALIGNMENT);
        const util::index_t frameOffset = metaDataBuffer().getAndAddInt32(
            LogBufferDescriptor::TERM_TAIL_COUNTER_OFFSET, alignedLength);
        AtomicBuffer&buffer = termBuffer();
        const std::int32_t capacity = buffer.capacity();

        std::int32_t resultingOffset = frameOffset + alignedLength;
        if (resultingOffset > (capacity - DataFrameHeader::LENGTH))
        {
            resultingOffset = handleEndOfLogCondition(buffer, frameOffset, capacity);
        }
        else
        {
            applyDefaultHeader(buffer, frameOffset, frameLength);
            buffer.putBytes(frameOffset + DataFrameHeader::LENGTH, srcBuffer, srcOffset, length);
            FrameDescriptor::frameLengthOrdered(buffer, frameOffset, frameLength);
        }

        return resultingOffset;
    }

    std::int32_t appendFragmentedMessage(AtomicBuffer& srcBuffer, util::index_t srcOffset, util::index_t length)
    {
        const int numMaxPayloads = length / m_maxPayloadLength;
        const util::index_t remainingPayload = length % m_maxPayloadLength;
        const util::index_t lastFrameLength = (remainingPayload > 0) ?
            util::BitUtil::align(remainingPayload + DataFrameHeader::LENGTH, FrameDescriptor::FRAME_ALIGNMENT) : 0;
        const util::index_t requiredLength = (numMaxPayloads * m_maxFrameLength) + lastFrameLength;
        util::index_t frameOffset = metaDataBuffer().getAndAddInt32(
            LogBufferDescriptor::TERM_TAIL_COUNTER_OFFSET, requiredLength);
        AtomicBuffer& buffer = termBuffer();
        const std::int32_t capacity = buffer.capacity();

        std::int32_t resultingOffset = frameOffset + requiredLength;
        if (resultingOffset > (capacity - DataFrameHeader::LENGTH))
        {
            resultingOffset = handleEndOfLogCondition(buffer, frameOffset, capacity);
        }
        else
        {
            std::uint8_t flags = FrameDescriptor::BEGIN_FRAG;
            util::index_t remaining = length;

            do
            {
                const util::index_t bytesToWrite = std::min(remaining, m_maxPayloadLength);
                const util::index_t frameLength = bytesToWrite + DataFrameHeader::LENGTH;
                const util::index_t alignedLength = util::BitUtil::align(frameLength, FrameDescriptor::FRAME_ALIGNMENT);

                applyDefaultHeader(buffer, frameOffset, frameLength);
                buffer.putBytes(
                    frameOffset + DataFrameHeader::LENGTH,
                    srcBuffer,
                    srcOffset + (length - remaining),
                    bytesToWrite);

                if (remaining <= m_maxPayloadLength)
                {
                    flags |= FrameDescriptor::END_FRAG;
                }

                FrameDescriptor::frameFlags(buffer, frameOffset, flags);
                FrameDescriptor::frameLengthOrdered(buffer, frameOffset, frameLength);

                flags = 0;
                frameOffset += alignedLength;
                remaining -= bytesToWrite;
            }
            while (remaining > 0);
        }

        return resultingOffset;
    }

    std::int32_t handleEndOfLogCondition(AtomicBuffer& termBuffer, util::index_t frameOffset, util::index_t capacity)
    {
        std::int32_t resultingOffset = TERM_APPENDER_FAILED;

        if (frameOffset <= (capacity - DataFrameHeader::LENGTH))
        {
            const std::int32_t paddingLength = capacity - frameOffset;
            applyDefaultHeader(termBuffer, frameOffset, paddingLength);
            FrameDescriptor::frameType(termBuffer, frameOffset, FrameDescriptor::PADDING_FRAME_TYPE);
            FrameDescriptor::frameLengthOrdered(termBuffer, frameOffset, paddingLength);

            resultingOffset = TERM_APPENDER_TRIPPED;
        }

        return resultingOffset;
    }

    void applyDefaultHeader(AtomicBuffer&termBuffer, util::index_t frameOffset, util::index_t frameLength)
    {
        termBuffer.putInt32Ordered(frameOffset, -frameLength);

        const std::int32_t termId = m_defaultHdr.getInt32(DataFrameHeader::TERM_ID_FIELD_OFFSET);

        struct DataFrameHeader::DataFrameHeaderDefn* hdr =
            (struct DataFrameHeader::DataFrameHeaderDefn *)(termBuffer.buffer() + frameOffset);

        hdr->version = DataFrameHeader::CURRENT_VERSION;
        hdr->flags = FrameDescriptor::BEGIN_FRAG | FrameDescriptor::END_FRAG;
        hdr->type = DataFrameHeader::HDR_TYPE_DATA;
        hdr->termOffset = frameOffset;
        hdr->sessionId = m_defaultHdrSessionId;
        hdr->streamId = m_defaultHdrStreamId;
        hdr->termId = termId;
    }
};

}}}

#endif
