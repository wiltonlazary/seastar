/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2019 ScyllaDB
 */


#include <seastar/core/file.hh>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/linux-aio.hh>
#include <seastar/core/internal/io_desc.hh>
#include <seastar/util/log.hh>
#include <chrono>
#include <mutex>
#include <array>
#include <fmt/format.h>
#include <fmt/ostream.h>

namespace seastar {

logger io_log("io");

using namespace std::chrono_literals;
using namespace internal::linux_abi;

class io_desc_read_write final : public io_completion {
    io_queue* _ioq_ptr;
    fair_queue_ticket _fq_ticket;
    promise<size_t> _pr;
private:
    void notify_requests_finished() noexcept {
        _ioq_ptr->notify_requests_finished(_fq_ticket);
    }
public:
    io_desc_read_write(io_queue* ioq, fair_queue_ticket ticket)
        : _ioq_ptr(ioq)
        , _fq_ticket(ticket)
    {}

    virtual void set_exception(std::exception_ptr eptr) noexcept override {
        io_log.trace("dev {} : req {} error", _ioq_ptr->dev_id(), fmt::ptr(this));
        notify_requests_finished();
        _pr.set_exception(eptr);
        delete this;
    }

    virtual void complete(size_t res) noexcept override {
        io_log.trace("dev {} : req {} complete", _ioq_ptr->dev_id(), fmt::ptr(this));
        notify_requests_finished();
        _pr.set_value(res);
        delete this;
    }

    future<size_t> get_future() {
        return _pr.get_future();
    }
};

void
io_queue::notify_requests_finished(fair_queue_ticket& desc) noexcept {
    _requests_executing--;
    _fq.notify_requests_finished(desc);
}

fair_queue::config io_queue::make_fair_queue_config(config iocfg) {
    fair_queue::config cfg;
    cfg.ticket_weight_pace = iocfg.disk_us_per_request / read_request_base_count;
    cfg.ticket_size_pace = (iocfg.disk_us_per_byte * (1 << request_ticket_size_shift)) / read_request_base_count;
    return cfg;
}

io_queue::io_queue(io_group_ptr group, io_queue::config cfg)
    : _priority_classes()
    , _group(std::move(group))
    , _fq(_group->_fg, make_fair_queue_config(cfg))
    , _config(std::move(cfg)) {
    seastar_logger.debug("Created io queue, multipliers {}:{}",
            cfg.disk_req_write_to_read_multiplier,
            cfg.disk_bytes_write_to_read_multiplier);
}

fair_group::config io_group::make_fair_group_config(config iocfg) noexcept {
    fair_group::config cfg;
    cfg.max_req_count = iocfg.max_req_count;
    cfg.max_bytes_count = iocfg.max_bytes_count >> io_queue::request_ticket_size_shift;
    return cfg;
}

io_group::io_group(config cfg) noexcept
    : _fg(make_fair_group_config(cfg)) {
    seastar_logger.debug("Created io group, limits {}:{}", cfg.max_req_count, cfg.max_bytes_count);
}

io_queue::~io_queue() {
    // It is illegal to stop the I/O queue with pending requests.
    // Technically we would use a gate to guarantee that. But here, it is not
    // needed since this is expected to be destroyed only after the reactor is destroyed.
    //
    // And that will happen only when there are no more fibers to run. If we ever change
    // that, then this has to change.
    for (auto&& pc_vec : _priority_classes) {
        for (auto&& pc_data : pc_vec) {
            if (pc_data) {
                _fq.unregister_priority_class(pc_data->ptr);
            }
        }
    }
}

std::mutex io_queue::_register_lock;
std::array<uint32_t, io_queue::_max_classes> io_queue::_registered_shares;
// We could very well just add the name to the io_priority_class. However, because that
// structure is passed along all the time - and sometimes we can't help but copy it, better keep
// it lean. The name won't really be used for anything other than monitoring.
std::array<sstring, io_queue::_max_classes> io_queue::_registered_names;

io_priority_class io_queue::register_one_priority_class(sstring name, uint32_t shares) {
    std::lock_guard<std::mutex> lock(_register_lock);
    for (unsigned i = 0; i < _max_classes; ++i) {
        if (!_registered_shares[i]) {
            _registered_shares[i] = shares;
            _registered_names[i] = std::move(name);
        } else if (_registered_names[i] != name) {
            continue;
        } else {
            // found an entry matching the name to be registered,
            // make sure it was registered with the same number shares
            // Note: those may change dynamically later on in the
            // fair queue priority_class_ptr
            assert(_registered_shares[i] == shares);
        }
        return io_priority_class(i);
    }
    throw std::runtime_error("No more room for new I/O priority classes");
}

bool io_queue::rename_one_priority_class(io_priority_class pc, sstring new_name) {
    std::lock_guard<std::mutex> guard(_register_lock);
    for (unsigned i = 0; i < _max_classes; ++i) {
       if (!_registered_shares[i]) {
           break;
       }
       if (_registered_names[i] == new_name) {
           if (i == pc.id()) {
               return false;
           } else {
               throw std::runtime_error(format("rename priority class: an attempt was made to rename a priority class to an"
                       " already existing name ({})", new_name));
           }
       }
    }
    _registered_names[pc.id()] = new_name;
    return true;
}

seastar::metrics::label io_queue_shard("ioshard");

io_queue::priority_class_data::priority_class_data(sstring name, sstring mountpoint, priority_class_ptr ptr, shard_id owner)
    : ptr(ptr)
    , bytes(0)
    , ops(0)
    , nr_queued(0)
    , queue_time(1s)
{
    register_stats(name, mountpoint, owner);
}

void
io_queue::priority_class_data::rename(sstring new_name, sstring mountpoint, shard_id owner) {
    try {
        register_stats(new_name, mountpoint, owner);
    } catch (metrics::double_registration &e) {
        // we need to ignore this exception, since it can happen that
        // a class that was already created with the new name will be
        // renamed again (this will cause a double registration exception
        // to be thrown).
    }

}

void
io_queue::priority_class_data::register_stats(sstring name, sstring mountpoint, shard_id owner) {
    seastar::metrics::metric_groups new_metrics;
    namespace sm = seastar::metrics;
    auto shard = sm::impl::shard();

    auto ioq_group = sm::label("mountpoint");
    auto mountlabel = ioq_group(mountpoint);

    auto class_label_type = sm::label("class");
    auto class_label = class_label_type(name);
    new_metrics.add_group("io_queue", {
            sm::make_derive("total_bytes", bytes, sm::description("Total bytes passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_operations", ops, sm::description("Total bytes passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            // Note: The counter below is not the same as reactor's queued-io-requests
            // queued-io-requests shows us how many requests in total exist in this I/O Queue.
            //
            // This counter lives in the priority class, so it will count only queued requests
            // that belong to that class.
            //
            // In other words: the new counter tells you how busy a class is, and the
            // old counter tells you how busy the system is.

            sm::make_queue_length("queue_length", nr_queued, sm::description("Number of requests in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_gauge("delay", [this] {
                return queue_time.count();
            }, sm::description("total delay time in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_gauge("shares", [this] {
                return this->ptr->shares();
            }, sm::description("current amount of shares"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label})
    });
    _metric_groups = std::exchange(new_metrics, {});
}

io_queue::priority_class_data& io_queue::find_or_create_class(const io_priority_class& pc, shard_id owner) {
    auto id = pc.id();
    bool do_insert = false;
    if ((do_insert = (owner >= _priority_classes.size()))) {
        _priority_classes.resize(owner + 1);
        _priority_classes[owner].resize(id + 1);
    } else if ((do_insert = (id >= _priority_classes[owner].size()))) {
        _priority_classes[owner].resize(id + 1);
    }
    if (do_insert || !_priority_classes[owner][id]) {
        auto shares = _registered_shares.at(id);
        sstring name;
        {
            std::lock_guard<std::mutex> lock(_register_lock);
            name = _registered_names.at(id);
        }

        // A note on naming:
        //
        // We could just add the owner as the instance id and have something like:
        //  io_queue-<class_owner>-<counter>-<class_name>
        //
        // However, when there are more than one shard per I/O queue, it is very useful
        // to know which shards are being served by the same queue. Therefore, a better name
        // scheme is:
        //
        //  io_queue-<queue_owner>-<counter>-<class_name>, shard=<class_owner>
        //  using the shard label to hold the owner number
        //
        // This conveys all the information we need and allows one to easily group all classes from
        // the same I/O queue (by filtering by shard)
        auto pc_ptr = _fq.register_priority_class(shares);
        auto pc_data = std::make_unique<priority_class_data>(name, mountpoint(), pc_ptr, owner);

        _priority_classes[owner][id] = std::move(pc_data);
    }
    return *_priority_classes[owner][id];
}

fair_queue_ticket io_queue::request_fq_ticket(const internal::io_request& req, size_t len) const {
    unsigned weight;
    size_t size;
    if (req.is_write()) {
        weight = _config.disk_req_write_to_read_multiplier;
        size = _config.disk_bytes_write_to_read_multiplier * len;
    } else if (req.is_read()) {
        weight = io_queue::read_request_base_count;
        size = io_queue::read_request_base_count * len;
    } else {
        throw std::runtime_error(fmt::format("Unrecognized request passing through I/O queue {}", req.opname()));
    }

    return fair_queue_ticket(weight, size >> request_ticket_size_shift);
}

future<size_t>
io_queue::queue_request(const io_priority_class& pc, size_t len, internal::io_request req) noexcept {
    auto start = std::chrono::steady_clock::now();
    return futurize_invoke([start, &pc, len, req = std::move(req), owner = this_shard_id(), this] () mutable {
        // First time will hit here, and then we create the class. It is important
        // that we create the shared pointer in the same shard it will be used at later.
        auto& pclass = find_or_create_class(pc, owner);
        fair_queue_ticket fq_ticket = request_fq_ticket(req, len);
        auto desc = std::make_unique<io_desc_read_write>(this, fq_ticket);
        auto fut = desc->get_future();
        io_log.trace("dev {} : req {} queue  len {} ticket {}", _config.devid, fmt::ptr(&*desc), len, fq_ticket);
        _fq.queue(pclass.ptr, std::move(fq_ticket), [&pclass, start, req = std::move(req), d = std::move(desc), len, this] () mutable noexcept {
            _queued_requests--;
            _requests_executing++;
            pclass.nr_queued--;
            pclass.ops++;
            pclass.bytes += len;
            pclass.queue_time = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start);
            io_log.trace("dev {} : req {} submit", _config.devid, fmt::ptr(&*d));
            engine().submit_io(d.release(), std::move(req));
        });
        pclass.nr_queued++;
        _queued_requests++;
        return fut;
    });
}

future<>
io_queue::update_shares_for_class(const io_priority_class pc, size_t new_shares) {
    return futurize_invoke([this, pc, owner = this_shard_id(), new_shares] {
        auto& pclass = find_or_create_class(pc, owner);
        pclass.ptr->update_shares(new_shares);
    });
}

void
io_queue::rename_priority_class(io_priority_class pc, sstring new_name) {
    for (unsigned owner = 0; owner < _priority_classes.size(); owner++) {
        if (_priority_classes[owner].size() > pc.id() &&
                _priority_classes[owner][pc.id()]) {
            _priority_classes[owner][pc.id()]->rename(new_name, _config.mountpoint, owner);
        }
    }
}

}
