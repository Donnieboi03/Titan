/**
 * Titan Python bindings (pybind11).
 * Links the C++ engine so Python can drive backtests.
 */
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <cstdint>
#include <string>

// Pull in engine headers (implementations are compiled separately)
#include "../core/order_engine.h"
#include "../core/engine_runtime.h"
#include "../core/market_data_stream.h"

namespace py = pybind11;
using namespace backtest;

// Helper to convert string to OrderSide
static engine::OrderSide order_side_from_string(const std::string& side) {
    if (side == "BID" || side == "bid" || side == "buy" || side == "BUY")
        return engine::OrderSide::BID;
    return engine::OrderSide::ASK;
}

// Accept side as str or OrderSide enum
static engine::OrderSide side_from_py(const py::object& side_obj) {
    if (py::isinstance<py::str>(side_obj))
        return order_side_from_string(side_obj.cast<std::string>());
    return side_obj.cast<engine::OrderSide>();
}

PYBIND11_MODULE(titan_core, m) {
    m.doc() = "Titan C++ core: multi-agent market microstructure backtesting engine";

    // --- Constants ---
    m.attr("INVALID_USER_ID") = py::int_(user::INVALID_USER_ID);
    m.attr("IPO_HOLDER") = py::int_(user::IPO_HOLDER);
    m.attr("INVALID_ORDER_ID") = py::int_(engine::INVALID_ORDER_ID);

    // --- Enums ---
    py::enum_<engine::EventKind>(m, "EventKind")
        .value("NONE",         engine::EventKind::NONE)
        .value("ACCEPT",       engine::EventKind::ACCEPT)
        .value("REJECT",       engine::EventKind::REJECT)
        .value("MODIFY",       engine::EventKind::MODIFY)
        .value("PARTIAL_FILL", engine::EventKind::PARTIAL_FILL)
        .value("FILL",         engine::EventKind::FILL)
        .value("CANCEL",       engine::EventKind::CANCEL)
        .export_values();

    py::enum_<engine::RejectReason>(m, "RejectReason")
        .value("NO_MARKET_LIQUIDITY", engine::RejectReason::NO_MARKET_LIQUIDITY)
        .value("ENGINE_FULL",         engine::RejectReason::ENGINE_FULL)
        .value("ORDER_NOT_FOUND",     engine::RejectReason::ORDER_NOT_FOUND)
        .export_values();

    py::enum_<engine::OrderSide>(m, "OrderSide")
        .value("BID", engine::OrderSide::BID)
        .value("ASK", engine::OrderSide::ASK)
        .export_values();
    
    py::enum_<engine::OrderStatus>(m, "OrderStatus")
        .value("OPEN", engine::OrderStatus::OPEN)
        .value("FILLED", engine::OrderStatus::FILLED)
        .value("CANCELLED", engine::OrderStatus::CANCELLED)
        .export_values();
    
    py::enum_<engine::OrderType>(m, "OrderType")
        .value("LIMIT", engine::OrderType::LIMIT)
        .value("MARKET", engine::OrderType::MARKET)
        .export_values();

    // --- OrderInfo (read-only struct) ---
    py::class_<engine::OrderInfo>(m, "OrderInfo")
        .def_readonly("price", &engine::OrderInfo::price_)
        .def_readonly("qty", &engine::OrderInfo::qty_)
        .def_readonly("side", &engine::OrderInfo::side_)
        .def_readonly("type", &engine::OrderInfo::type_)
        .def_readonly("status", &engine::OrderInfo::status_)
        .def_readonly("time", &engine::OrderInfo::time_)
        .def("get_price_dollars", [](const engine::OrderInfo& self) {
            return backtest::math::ticks_to_dollars(self.price_);
        })
        .def("get_qty", [](const engine::OrderInfo& self) {
            return backtest::math::internal_to_qty(self.qty_);
        })
        .def("__repr__", [](const engine::OrderInfo& self) {
            std::string side = (self.side_ == engine::OrderSide::BID) ? "BID" : "ASK";
            std::string type = (self.type_ == engine::OrderType::LIMIT) ? "LIMIT" : "MARKET";
            std::string status;
            switch (self.status_) {
                case engine::OrderStatus::OPEN:      status = "OPEN"; break;
                case engine::OrderStatus::FILLED:    status = "FILLED"; break;
                case engine::OrderStatus::CANCELLED: status = "CANCELLED"; break;
                default:                             status = "NONE"; break;
            }
            return "<OrderInfo " + side + " " + type + " $" +
                   std::to_string(backtest::math::ticks_to_dollars(self.price_)) +
                   " x " + std::to_string(backtest::math::internal_to_qty(self.qty_)) +
                   " [" + status + "]>";
        });

    // --- UserSnapshot (read-only copy for Python; double-buffered in C++; single-ticker per strategy) ---
    py::class_<user::UserSnapshot>(m, "UserSnapshot")
        .def_readonly("user_id", &user::UserSnapshot::user_id)
        .def_readonly("capital", &user::UserSnapshot::capital)
        .def_readonly("realized_pnl", &user::UserSnapshot::realized_pnl)
        .def_readonly("total_volume", &user::UserSnapshot::total_volume)
        .def_readonly("ticker", &user::UserSnapshot::ticker)
        .def_readonly("position", &user::UserSnapshot::position)
        .def_readonly("avg_price", &user::UserSnapshot::avg_price)
        .def_readonly("unrealized_pnl", &user::UserSnapshot::unrealized_pnl);

    // --- UserView (observational handle returned by register_strategy; no submit_*) ---
    py::class_<user::UserView>(m, "UserView")
        .def("get_snapshot",
             [](const user::UserView& self) { return self.get_snapshot(); },
             py::return_value_policy::copy,
             "Return a copy of the latest user state snapshot (capital, positions, etc.).")
        .def("get_capital", &user::UserView::get_capital)
        .def("get_realized_pnl", &user::UserView::get_realized_pnl)
        .def("get_total_volume", &user::UserView::get_total_volume)
        .def("get_user_id", &user::UserView::get_user_id)
        .def("get_ticker", &user::UserView::get_ticker)
        .def("get_position", &user::UserView::get_position)
        .def("get_all_positions", &user::UserView::get_all_positions)
        .def("get_committed_sell_qty", &user::UserView::get_committed_sell_qty)
        .def("get_unrealized_pnl", &user::UserView::get_unrealized_pnl);

    // --- User (full handle passed to strategy callback; extends UserView, no ticker param — uses registered ticker) ---
    py::class_<user::User, user::UserView>(m, "User")
        .def("submit_limit_order",
             [](user::User& self, const py::object& side, double price, double quantity) {
                 return self.submit_limit_order(side_from_py(side), price, quantity);
             },
             py::arg("side"), py::arg("price"), py::arg("quantity"),
             py::call_guard<py::gil_scoped_release>())
        .def("submit_market_order",
             [](user::User& self, const py::object& side, double quantity) {
                 return self.submit_market_order(side_from_py(side), quantity);
             },
             py::arg("side"), py::arg("quantity"),
             py::call_guard<py::gil_scoped_release>())
        .def("submit_cancel_order", &user::User::submit_cancel_order,
             py::arg("order_id"),
             py::call_guard<py::gil_scoped_release>())
        .def("submit_replace_order", &user::User::submit_replace_order,
             py::arg("order_id"), py::arg("new_price"), py::arg("new_quantity"),
             py::call_guard<py::gil_scoped_release>())
        .def("submit_edit_order", &user::User::submit_edit_order,
             py::arg("order_id"), py::arg("new_quantity"),
             py::call_guard<py::gil_scoped_release>())
        .def("get_best_bid", &user::User::get_best_bid)
        .def("get_best_ask", &user::User::get_best_ask)
        .def("get_market_price", &user::User::get_market_price)
        .def("get_market_depth",
             [](user::User& self, const py::object& side, std::size_t depth) {
                 return self.get_market_depth(side_from_py(side), depth);
             },
             py::arg("side"), py::arg("depth") = 20)
        .def("list_tickers", &user::User::list_tickers)
        .def("get_positions", &user::User::get_positions)
        .def("get_active_orders", &user::User::get_active_orders)
        .def("get_position", &user::User::get_position)
        .def("get_all_positions", &user::User::get_all_positions)
        .def("get_order_info",
             [](const user::User& self, engine::OrderId order_id) -> py::object {
                 const engine::OrderInfo* info = self.get_order_info(order_id);
                 if (!info) return py::none();
                 engine::OrderInfo copy(*info);
                 return py::cast(copy);
             },
             py::arg("order_id"))
        .def("has_sufficient_shares",
             [](const user::User& self, double qty) {
                 return self.has_sufficient_shares(backtest::math::qty_to_internal(qty));
             },
             py::arg("qty"))
        
        // Account info
        .def("get_user_id", &user::User::get_user_id)
        .def("get_capital", &user::User::get_capital)
        .def("get_realized_pnl", &user::User::get_realized_pnl)
        .def("get_unrealized_pnl", &user::UserView::get_unrealized_pnl)
        .def("get_total_volume", &user::User::get_total_volume)
        .def("get_open_bids", &user::User::get_open_bids)
        .def("get_open_asks", &user::User::get_open_asks);

    // get_order: return a copy (or None) so Python can hold it safely; C++ returns pointer
    auto get_order_fn = [](runtime::EngineRuntime& self, const std::string& ticker, engine::OrderId order_id) -> py::object {
        const engine::OrderInfo* p = self.get_order(ticker, order_id);
        if (!p) return py::none();
        return py::cast(engine::OrderInfo(*p));
    };

    // --- EngineRuntime (singleton with private destructor - use nodelete) ---
    py::class_<runtime::EngineRuntime, std::unique_ptr<runtime::EngineRuntime, py::nodelete>>(m, "EngineRuntime")
        .def_static("get_instance",
             [](std::size_t num_threads, bool verbose, std::size_t quantum, std::size_t max_capacity, std::size_t max_engine_count, std::size_t max_strategies)
             -> runtime::EngineRuntime& {
                 return runtime::EngineRuntime::get_instance(num_threads, verbose, quantum, max_capacity, max_engine_count, max_strategies);
             },
             py::arg("num_threads") = 1,
             py::arg("verbose") = false,
             py::arg("quantum") = 1000,
             py::arg("max_capacity") = 1048576,
             py::arg("max_engine_count") = 100,
             py::arg("max_strategies") = 1000,
             py::return_value_policy::reference)
        .def_static("reset_instance", &runtime::EngineRuntime::reset_instance)
        
        // Stock registration (C++ takes std::string&&; bindings take by value and move)
        .def("register_stock",
             [](runtime::EngineRuntime& self, std::string ticker, double ipo_price, double ipo_qty, std::size_t capacity) {
                 return self.register_stock(std::move(ticker), ipo_price, ipo_qty, capacity);
             },
             py::arg("ticker"), py::arg("ipo_price"), py::arg("ipo_qty"), 
             py::arg("capacity") = 0,
             py::call_guard<py::gil_scoped_release>())
        .def("unregister_stock",
             [](runtime::EngineRuntime& self, std::string ticker) {
                 return self.unregister_stock(std::move(ticker));
             },
             py::arg("ticker"),
             py::call_guard<py::gil_scoped_release>())
        
        // Order submission (untracked only). For tracked orders use the User view's submit_*.
        .def("submit_limit_order",
             [](runtime::EngineRuntime& self, const std::string& ticker, const py::object& side,
                double price, double qty) {
                 return self.submit_limit_order(ticker, side_from_py(side), price, qty);
             },
             py::arg("ticker"), py::arg("side"), py::arg("price"), py::arg("qty"),
             py::call_guard<py::gil_scoped_release>())
        .def("submit_market_order",
             [](runtime::EngineRuntime& self, const std::string& ticker, const py::object& side,
                double qty) {
                 return self.submit_market_order(ticker, side_from_py(side), qty);
             },
             py::arg("ticker"), py::arg("side"), py::arg("qty"),
             py::call_guard<py::gil_scoped_release>())
        .def("submit_cancel_order", &runtime::EngineRuntime::submit_cancel_order,
             py::arg("ticker"), py::arg("order_id"),
             py::call_guard<py::gil_scoped_release>())
        .def("submit_replace_order", &runtime::EngineRuntime::submit_replace_order,
             py::arg("ticker"), py::arg("order_id"), py::arg("new_price"), py::arg("new_qty"),
             py::call_guard<py::gil_scoped_release>())
        .def("submit_edit_order", &runtime::EngineRuntime::submit_edit_order,
             py::arg("ticker"), py::arg("order_id"), py::arg("new_qty"),
             py::call_guard<py::gil_scoped_release>())
        
        // Market data queries
        .def("get_market_price", &runtime::EngineRuntime::get_market_price, py::arg("ticker"))
        .def("get_best_bid", &runtime::EngineRuntime::get_best_bid, py::arg("ticker"))
        .def("get_best_ask", &runtime::EngineRuntime::get_best_ask, py::arg("ticker"))
        .def("get_market_depth",
             [](runtime::EngineRuntime& self, const std::string& ticker,
                const py::object& side, std::size_t depth) {
                 return self.get_market_depth(ticker, side_from_py(side), depth);
             },
             py::arg("ticker"), py::arg("side"), py::arg("depth") = 20)
        .def("list_tickers", &runtime::EngineRuntime::list_tickers)
        
        // Batch processing
        .def("request_snapshot", &runtime::EngineRuntime::request_snapshot, py::arg("ticker"))
        .def("process_pending_orders", 
             py::overload_cast<>(&runtime::EngineRuntime::process_pending_orders),
             py::call_guard<py::gil_scoped_release>())
        .def("process_pending_orders", 
             py::overload_cast<const std::string&>(&runtime::EngineRuntime::process_pending_orders),
             py::arg("ticker"),
             py::call_guard<py::gil_scoped_release>())
        .def("process_pending_orders_async",
             py::overload_cast<>(&runtime::EngineRuntime::process_pending_orders_async),
             py::call_guard<py::gil_scoped_release>())
        .def("process_pending_orders_async",
             py::overload_cast<const std::string&>(&runtime::EngineRuntime::process_pending_orders_async),
             py::arg("ticker"),
             py::call_guard<py::gil_scoped_release>())
        // Simulation (internal parser loop; C++ takes std::string&&)
        .def("simulate",
             [](runtime::EngineRuntime& self, std::string filepath, std::string ticker,
                std::size_t target_orders, std::size_t price_sample_size, double shares_outstanding, std::string record_path) {
                 return self.simulate(std::move(filepath), std::move(ticker), target_orders, price_sample_size, shares_outstanding, std::move(record_path));
             },
             py::arg("filepath"), py::arg("ticker"),
             py::arg("target_orders") = 0,
             py::arg("price_sample_size") = 10,
             py::arg("shares_outstanding") = 1000000.0,
             py::arg("record_path") = "",
             py::call_guard<py::gil_scoped_release>(),
             py::doc("Start async simulation. Returns True if started. Poll is_simulation_running(ticker) and get_simulation_metrics(ticker)."))
        .def("is_simulation_running", &runtime::EngineRuntime::is_simulation_running, py::arg("ticker"))
        .def("get_simulation_metrics", &runtime::EngineRuntime::get_simulation_metrics, py::arg("ticker"))
        .def("all_jobs_completed", &runtime::EngineRuntime::all_jobs_completed)
        
        // Control
        .def("set_auto_match", &runtime::EngineRuntime::set_auto_match,
             py::arg("ticker"), py::arg("auto_match"),
             py::doc("Enable or disable automatic matching for ticker (advanced)."))
        .def("get_auto_match", &runtime::EngineRuntime::get_auto_match, py::arg("ticker"))
        .def("set_batch_size", &runtime::EngineRuntime::set_batch_size, py::arg("batch_size"))
        .def("get_batch_size", &runtime::EngineRuntime::get_batch_size)
        .def("get_quantum", &runtime::EngineRuntime::get_quantum)
        .def("set_notify_order", &runtime::EngineRuntime::set_notify_order, py::arg("enable"))
        .def("get_notify_order", &runtime::EngineRuntime::get_notify_order)
        .def("set_record",
             [](runtime::EngineRuntime& self, std::string ticker, bool enable) {
                 self.set_record(std::move(ticker), enable);
             },
             py::arg("ticker"), py::arg("enable"))
        .def("set_record",
             [](runtime::EngineRuntime& self, std::string ticker, bool enable, std::string path_override) {
                 self.set_record(std::move(ticker), enable, std::move(path_override));
             },
             py::arg("ticker"), py::arg("enable"), py::arg("path_override"))
        .def("get_record", &runtime::EngineRuntime::get_record, py::arg("ticker"))
        
        // Statistics
        .def("get_placed_count", &runtime::EngineRuntime::get_placed_count, py::arg("ticker"))
        .def("get_cancelled_count", &runtime::EngineRuntime::get_cancelled_count, py::arg("ticker"))
        .def("get_filled_count", &runtime::EngineRuntime::get_filled_count, py::arg("ticker"))
        .def("get_open_count", &runtime::EngineRuntime::get_open_count, py::arg("ticker"))
        .def("get_order", get_order_fn, py::arg("ticker"), py::arg("order_id"))
        // Diagnostics
        .def("get_capacity", &runtime::EngineRuntime::get_capacity, py::arg("ticker"))
        .def("get_pending_count", &runtime::EngineRuntime::get_pending_count, py::arg("ticker"))
        .def("get_positions",
             [](const runtime::EngineRuntime& self, user::UserId user_id, const std::string& ticker) {
                 return self.get_positions(user_id, ticker);
             },
             py::arg("user_id"), py::arg("ticker"))
        .def("get_active_orders",
             [](const runtime::EngineRuntime& self, user::UserId user_id, const std::string& ticker) {
                 return self.get_active_orders(user_id, ticker);
             },
             py::arg("user_id"), py::arg("ticker"))
        
        // Strategy management
        .def("unregister_strategy", &runtime::EngineRuntime::unregister_strategy,
             py::arg("user_id"))

        // Strategy registration (ticker required for deterministic per-engine quantum; C++ takes std::string&&)
        // Returns UserView* so Python has observational API; for tracked orders use the view's submit_* methods.
        .def("register_strategy",
             ([](runtime::EngineRuntime& self, std::string ticker, py::function py_strategy, double starting_capital) -> user::UserView* {
                 user::Strategy cpp_strategy = [py_strategy](user::User* user) {
                     py::gil_scoped_acquire gil;
                     try {
                         py_strategy(user);
                     } catch (const py::error_already_set& e) {
                         std::cerr << "Python strategy error: " << e.what() << "\n";
                     }
                 };
                 return self.register_strategy(std::move(ticker), std::move(cpp_strategy), starting_capital);
             }),
             py::arg("ticker"), py::arg("strategy"), py::arg("starting_capital") = 100000.0,
             py::return_value_policy::reference);

    // --- SimulationMetrics (read-only struct for get_simulation_metrics) ---
    py::class_<runtime::SimulationMetrics>(m, "SimulationMetrics")
        .def_readonly("market_updates_processed", &runtime::SimulationMetrics::market_updates_processed)
        .def_readonly("orders_placed", &runtime::SimulationMetrics::orders_placed)
        .def_readonly("orders_filled", &runtime::SimulationMetrics::orders_filled)
        .def_readonly("orders_cancelled", &runtime::SimulationMetrics::orders_cancelled)
        .def_readonly("simulation_time_seconds", &runtime::SimulationMetrics::simulation_time_seconds)
        .def_property_readonly("orders_per_second", [](const runtime::SimulationMetrics& m) { return m.orders_per_second(); })
        .def_property_readonly("updates_per_second", [](const runtime::SimulationMetrics& m) { return m.updates_per_second(); })
        .def_readonly("peak_open_orders", &runtime::SimulationMetrics::peak_open_orders)
        .def_readonly("final_open_orders", &runtime::SimulationMetrics::final_open_orders)
        .def_readonly("initial_price", &runtime::SimulationMetrics::initial_price)
        .def_readonly("final_price", &runtime::SimulationMetrics::final_price)
        .def_readonly("cache_entries", &runtime::SimulationMetrics::cache_entries)
        .def_readonly("simulation_running", &runtime::SimulationMetrics::simulation_running);

    // --- L2Stream (read/write L2 market data) ---
    py::class_<stream::L2Stream>(m, "L2Stream")
        .def(py::init<const std::string&, bool>(), py::arg("filepath"), py::arg("streaming") = true)
        .def(py::init<const std::string&, stream::StreamMode>(), py::arg("filepath"), py::arg("mode"))
        .def("parse_next",
             [](stream::L2Stream& self) -> py::object {
                 stream::L2Update update;
                 bool ok = false;
                 {
                     py::gil_scoped_release release;
                     ok = self.parse_next(update);
                 }
                 if (!ok) {
                     return py::none();
                 }
                 py::dict result;
                 result["timestamp"] = update.timestamp;
                 result["price"] = update.price;
                 result["amount"] = update.amount;
                 result["side"] = std::string(1, update.side);
                 result["is_snapshot"] = update.is_snapshot;
                 return result;
             })
        .def("get_total_records", &stream::L2Stream::get_total_records)
        .def("is_open", &stream::L2Stream::is_open)
        .def("write",
             [](stream::L2Stream& self, const py::dict& d) {
                 stream::L2Update u;
                 u.timestamp = d.contains("timestamp") ? d["timestamp"].cast<int64_t>() : 0;
                 u.price = d.contains("price") ? d["price"].cast<double>() : 0.0;
                 u.amount = d.contains("amount") ? d["amount"].cast<double>() : 0.0;
                 std::string s = d.contains("side") ? d["side"].cast<std::string>() : "b";
                 u.side = s.empty() ? 'b' : s[0];
                 u.is_snapshot = d.contains("is_snapshot") && d["is_snapshot"].cast<bool>();
                 return self.write(u);
             }, py::arg("update"))
        .def("flush", &stream::L2Stream::flush)
        .def("close", [](stream::L2Stream& self) {
             (void)self;
             });
    py::enum_<stream::StreamMode>(m, "StreamMode")
        .value("Read", stream::StreamMode::Read)
        .value("Write", stream::StreamMode::Write)
        .export_values();
}

