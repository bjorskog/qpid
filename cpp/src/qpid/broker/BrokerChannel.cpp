/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */
#include <assert.h>

#include <iostream>
#include <sstream>
#include <algorithm>
#include <functional>

#include <boost/bind.hpp>
#include <boost/format.hpp>

#include "qpid/QpidError.h"

#include "BrokerAdapter.h"
#include "BrokerChannel.h"
#include "BrokerMessage.h"
#include "BrokerQueue.h"
#include "Connection.h"
#include "DeliverableMessage.h"
#include "DtxAck.h"
#include "DtxTimeout.h"
#include "MessageStore.h"
#include "TxAck.h"
#include "TxPublish.h"

using std::mem_fun_ref;
using std::bind2nd;
using namespace qpid::broker;
using namespace qpid::framing;
using namespace qpid::sys;


Channel::Channel(Connection& con, DeliveryAdapter& _out, ChannelId _id, MessageStore* const _store) :
    id(_id),
    connection(con),
    out(_out),
    currentDeliveryTag(1),
    prefetchSize(0),
    prefetchCount(0),
    tagGenerator("sgen"),
    dtxSelected(false),
    accumulatedAck(0),
    store(_store),
    messageBuilder(this, _store, connection.getStagingThreshold()),
    opened(id == 0),//channel 0 is automatically open, other must be explicitly opened
    flowActive(true)
{
    outstanding.reset();
}

Channel::~Channel(){
    close();
}

bool Channel::exists(const string& consumerTag){
    return consumers.find(consumerTag) != consumers.end();
}

// TODO aconway 2007-02-12: Why is connection token passed in instead
// of using the channel's parent connection?
void Channel::consume(DeliveryToken::shared_ptr token, string& tagInOut, 
                      Queue::shared_ptr queue, bool acks,
                      bool exclusive, ConnectionToken* const connection,
                      const FieldTable*)
{
    if(tagInOut.empty())
        tagInOut = tagGenerator.generate();
    std::auto_ptr<ConsumerImpl> c(
    new ConsumerImpl(this, token, tagInOut, queue, connection, acks));
    queue->consume(c.get(), exclusive);//may throw exception
    consumers.insert(tagInOut, c.release());
}

void Channel::cancel(const string& tag){
    // consumers is a ptr_map so erase will delete the consumer
    // which will call cancel.
    ConsumerImplMap::iterator i = consumers.find(tag);
    if (i != consumers.end())
        consumers.erase(i); 
}

void Channel::close()
{
    opened = false;
    consumers.clear();
    if (dtxBuffer.get()) {
        dtxBuffer->fail();
    }
    recover(true);
}

void Channel::startTx()
{
    txBuffer = TxBuffer::shared_ptr(new TxBuffer());
}

void Channel::commit()
{
    if (!txBuffer) throw ConnectionException(503, "Channel has not been selected for use with transactions");

    TxOp::shared_ptr txAck(new TxAck(accumulatedAck, unacked));
    txBuffer->enlist(txAck);
    if (txBuffer->commitLocal(store)) {
        accumulatedAck.clear();
    }
}

void Channel::rollback()
{
    if (!txBuffer) throw ConnectionException(503, "Channel has not been selected for use with transactions");

    txBuffer->rollback();
    accumulatedAck.clear();
}

void Channel::selectDtx()
{
    dtxSelected = true;
}

void Channel::startDtx(const std::string& xid, DtxManager& mgr, bool join)
{
    if (!dtxSelected) {
        throw ConnectionException(503, "Channel has not been selected for use with dtx");
    }
    dtxBuffer = DtxBuffer::shared_ptr(new DtxBuffer(xid));
    txBuffer = static_pointer_cast<TxBuffer>(dtxBuffer);
    if (join) {
        mgr.join(xid, dtxBuffer);
    } else {
        mgr.start(xid, dtxBuffer);
    }
}

void Channel::endDtx(const std::string& xid, bool fail)
{
    if (!dtxBuffer) {
        throw ConnectionException(503, boost::format("xid %1% not associated with this channel") % xid);
    }
    if (dtxBuffer->getXid() != xid) {
        throw ConnectionException(503, boost::format("xid specified on start was %1%, but %2% specified on end") 
                                  % dtxBuffer->getXid() % xid);
    }

    txBuffer.reset();//ops on this channel no longer transactional

    checkDtxTimeout();
    if (fail) {
        dtxBuffer->fail();
    } else {
        dtxBuffer->markEnded();
    }    
    dtxBuffer.reset();
}

void Channel::suspendDtx(const std::string& xid)
{
    if (dtxBuffer->getXid() != xid) {
        throw ConnectionException(503, boost::format("xid specified on start was %1%, but %2% specified on suspend") 
                                  % dtxBuffer->getXid() % xid);
    }
    txBuffer.reset();//ops on this channel no longer transactional

    checkDtxTimeout();
    dtxBuffer->setSuspended(true);
}

void Channel::resumeDtx(const std::string& xid)
{
    if (dtxBuffer->getXid() != xid) {
        throw ConnectionException(503, boost::format("xid specified on start was %1%, but %2% specified on resume") 
                                  % dtxBuffer->getXid() % xid);
    }
    if (!dtxBuffer->isSuspended()) {
        throw ConnectionException(503, boost::format("xid %1% not suspended")% xid);
    }

    checkDtxTimeout();
    dtxBuffer->setSuspended(false);
    txBuffer = static_pointer_cast<TxBuffer>(dtxBuffer);
}

void Channel::checkDtxTimeout()
{
    if (dtxBuffer->isExpired()) {
        dtxBuffer.reset();
        throw DtxTimeoutException();
    }
}

void Channel::record(const DeliveryRecord& delivery)
{
    unacked.push_back(delivery);
    delivery.addTo(&outstanding);
}

bool Channel::checkPrefetch(Message::shared_ptr& msg)
{
    Mutex::ScopedLock locker(deliveryLock);
    bool countOk = !prefetchCount || prefetchCount > unacked.size();
    bool sizeOk = !prefetchSize || prefetchSize > msg->contentSize() + outstanding.size || unacked.empty();
    return countOk && sizeOk;
}

Channel::ConsumerImpl::ConsumerImpl(Channel* _parent, DeliveryToken::shared_ptr _token,
                                    const string& _tag, Queue::shared_ptr _queue, 
                                    ConnectionToken* const _connection, bool ack
                                    ) : parent(_parent), token(_token), tag(_tag), queue(_queue), connection(_connection),
                                        ackExpected(ack), blocked(false) {}

bool Channel::ConsumerImpl::deliver(Message::shared_ptr& msg)
{
    if(!connection || connection != msg->getPublisher()){//check for no_local
        if(!parent->flowActive || (ackExpected && !parent->checkPrefetch(msg))){
            blocked = true;
        }else{
            blocked = false;
            Mutex::ScopedLock locker(parent->deliveryLock);

            uint64_t deliveryTag = parent->out.deliver(msg, token);
            if(ackExpected){
                parent->record(DeliveryRecord(msg, queue, tag, deliveryTag));
            }

            return true;
        }
    }
    return false;
}

void Channel::ConsumerImpl::redeliver(Message::shared_ptr& msg, uint64_t deliveryTag) {
    Mutex::ScopedLock locker(parent->deliveryLock);
    parent->out.redeliver(msg, token, deliveryTag);
}

Channel::ConsumerImpl::~ConsumerImpl() {
    cancel();
}

void Channel::ConsumerImpl::cancel()
{
    if(queue) {
        queue->cancel(this);
        if (queue->canAutoDelete()) {            
            parent->connection.broker.getQueues().destroyIf(queue->getName(), 
                                                            boost::bind(boost::mem_fn(&Queue::canAutoDelete), queue));
        }
    }
}

void Channel::ConsumerImpl::requestDispatch()
{
    if(blocked)
        queue->requestDispatch();
}

void Channel::handleInlineTransfer(Message::shared_ptr msg)
{
    complete(msg);
}

void Channel::handlePublish(Message* _message)
{
    Message::shared_ptr message(_message);
    messageBuilder.initialise(message);
}

void Channel::handleHeader(AMQHeaderBody::shared_ptr header)
{
    messageBuilder.setHeader(header);
    //at this point, decide based on the size of the message whether we want
    //to stage it by saving content directly to disk as it arrives
}

void Channel::handleContent(AMQContentBody::shared_ptr content)
{
    messageBuilder.addContent(content);
}

void Channel::handleHeartbeat(boost::shared_ptr<AMQHeartbeatBody>) {
    // TODO aconway 2007-01-17: Implement heartbeating.
}

void Channel::complete(Message::shared_ptr msg) {
    if (txBuffer.get()) {
        TxPublish* deliverable(new TxPublish(msg));
        TxOp::shared_ptr op(deliverable);
        route(msg, *deliverable);
        txBuffer->enlist(op);
    } else {
        DeliverableMessage deliverable(msg);
        route(msg, deliverable);
    }
}

void Channel::route(Message::shared_ptr msg, Deliverable& strategy) {
    Exchange::shared_ptr exchange = connection.broker.getExchanges().get(msg->getExchange());
    assert(exchange.get());
    exchange->route(strategy, msg->getRoutingKey(), &(msg->getApplicationHeaders()));
    if (!strategy.delivered) {
        //TODO:if reject-unroutable, then reject
        //else route to alternate exchange
        if (exchange->getAlternate()) {
            exchange->getAlternate()->route(strategy, msg->getRoutingKey(), &(msg->getApplicationHeaders()));
        }
    }

}

// Used by Basic
void Channel::ack(uint64_t deliveryTag, bool multiple)
{
    if (multiple)
        ack(0, deliveryTag);
    else
        ack(deliveryTag, deliveryTag);
}

void Channel::ack(uint64_t firstTag, uint64_t lastTag)
{
    if (txBuffer.get()) {
        accumulatedAck.update(firstTag, lastTag);
        //TODO: I think the outstanding prefetch size & count should be updated at this point...
        //TODO: ...this may then necessitate dispatching to consumers
        if (dtxBuffer.get()) {
            TxOp::shared_ptr txAck(new DtxAck(accumulatedAck, unacked));
            accumulatedAck.clear();
            dtxBuffer->enlist(txAck);    
        }

    } else {
        Mutex::ScopedLock locker(deliveryLock);//need to synchronize with possible concurrent delivery
    
        ack_iterator i = find_if(unacked.begin(), unacked.end(), bind2nd(mem_fun_ref(&DeliveryRecord::matches), lastTag));
        ack_iterator j = (firstTag == 0) ?
            unacked.begin() :
            find_if(unacked.begin(), unacked.end(), bind2nd(mem_fun_ref(&DeliveryRecord::matches), firstTag));
        	
        if(i == unacked.end()){
            throw ConnectionException(530, "Received ack for unrecognised delivery tag");
        }else if(i!=j){
            ack_iterator end = ++i;
            for_each(j, end, bind2nd(mem_fun_ref(&DeliveryRecord::discard), 0));
            unacked.erase(unacked.begin(), end);

            //recalculate the prefetch:
            outstanding.reset();
            for_each(unacked.begin(), unacked.end(), bind2nd(mem_fun_ref(&DeliveryRecord::addTo), &outstanding));
        }else{
            i->discard();
            i->subtractFrom(&outstanding);
            unacked.erase(i);        
        }

        //if the prefetch limit had previously been reached, there may
        //be messages that can be now be delivered
        std::for_each(consumers.begin(), consumers.end(),
                      boost::bind(&ConsumerImpl::requestDispatch, _1));
    }
}

void Channel::recover(bool requeue)
{
    Mutex::ScopedLock locker(deliveryLock);//need to synchronize with possible concurrent delivery

    if(requeue){
        outstanding.reset();
        std::list<DeliveryRecord> copy = unacked;
        unacked.clear();
        for_each(copy.rbegin(), copy.rend(), mem_fun_ref(&DeliveryRecord::requeue));
    }else{
        for_each(unacked.begin(), unacked.end(), bind2nd(mem_fun_ref(&DeliveryRecord::redeliver), this));        
    }
}

bool Channel::get(DeliveryToken::shared_ptr token, Queue::shared_ptr queue, bool ackExpected)
{
    Message::shared_ptr msg = queue->dequeue();
    if(msg){
        Mutex::ScopedLock locker(deliveryLock);
        uint64_t myDeliveryTag = out.deliver(msg, token);
        if(ackExpected){
            unacked.push_back(DeliveryRecord(msg, queue, myDeliveryTag));
        }
        return true;
    }else{
        return false;
    }
}

void Channel::deliver(Message::shared_ptr& msg, const string& consumerTag,
                      uint64_t deliveryTag)
{
    ConsumerImplMap::iterator i = consumers.find(consumerTag);
    if (i != consumers.end()){
        i->redeliver(msg, deliveryTag);
    }
}

void Channel::flow(bool active)
{
    Mutex::ScopedLock locker(deliveryLock);
    bool requestDelivery(!flowActive && active);
    flowActive = active;
    if (requestDelivery) {
        //there may be messages that can be now be delivered
        std::for_each(consumers.begin(), consumers.end(), boost::bind(&ConsumerImpl::requestDispatch, _1));
    }
}
