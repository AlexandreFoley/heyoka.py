// Copyright 2020, 2021, 2022 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka.py library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <boost/numeric/conversion/cast.hpp>

#include <fmt/format.h>

#include <oneapi/tbb/global_control.h>

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#define PY_ARRAY_UNIQUE_SYMBOL heyoka_py_ARRAY_API
#define PY_UFUNC_UNIQUE_SYMBOL heyoka_py_UFUNC_API
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include <Python.h>
#include <numpy/arrayobject.h>
#include <numpy/ufuncobject.h>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/celmec/vsop2013.hpp>
#include <heyoka/exceptions.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/mascon.hpp>
#include <heyoka/math.hpp>
#include <heyoka/nbody.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>

#include "cfunc.hpp"
#include "common_utils.hpp"
#include "custom_casters.hpp"
#include "dtypes.hpp"
#include "expose_M2E.hpp"
#include "expose_expression.hpp"
#include "expose_real128.hpp"
#include "logging.hpp"
#include "pickle_wrappers.hpp"
#include "setup_sympy.hpp"
#include "taylor_add_jet.hpp"
#include "taylor_expose_c_output.hpp"
#include "taylor_expose_events.hpp"
#include "taylor_expose_integrator.hpp"

namespace py = pybind11;
namespace hey = heyoka;
namespace heypy = heyoka_py;

namespace heyoka_py::detail
{

namespace
{

std::optional<oneapi::tbb::global_control> tbb_gc;

// Helper to import the NumPy API bits.
PyObject *import_numpy(PyObject *m)
{
    import_array();

    import_umath();

    return m;
}

} // namespace

} // namespace heyoka_py::detail

#if defined(__clang__)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"

#endif

PYBIND11_MODULE(core, m)
{
    // Import the NumPy API bits.
    if (heypy::detail::import_numpy(m.ptr()) == nullptr) {
        // NOTE: on failure, the NumPy macros already set
        // the error indicator. Thus, all it is left to do
        // is to throw the pybind11 exception.
        throw py::error_already_set();
    }

    using namespace pybind11::literals;
    namespace kw = hey::kw;

    m.doc() = "The core heyoka module";

    // Flag PPC arch.
    m.attr("_ppc_arch") =
#if defined(HEYOKA_ARCH_PPC)
        true
#else
        false
#endif
        ;

    // Expose the real128 type.
    heypy::expose_real128(m);

    // Expose the logging setter functions.
    heypy::expose_logging_setters(m);

    // Export the heyoka version.
    m.attr("_heyoka_cpp_version_major") = HEYOKA_VERSION_MAJOR;
    m.attr("_heyoka_cpp_version_minor") = HEYOKA_VERSION_MINOR;
    m.attr("_heyoka_cpp_version_patch") = HEYOKA_VERSION_PATCH;

    // Register heyoka's custom exceptions.
    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) {
                std::rethrow_exception(p);
            }
        } catch (const hey::not_implemented_error &nie) {
            PyErr_SetString(PyExc_NotImplementedError, nie.what());
        }
    });

    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) {
                std::rethrow_exception(p);
            }
        } catch (const hey::zero_division_error &zde) {
            PyErr_SetString(PyExc_ZeroDivisionError, zde.what());
        }
    });

    // Expression.
    heypy::expose_expression(m);

    // M2E.
    heypy::expose_M2E(m);

    // N-body builders.
    m.def(
        "make_nbody_sys",
        [](std::uint32_t n, py::object Gconst, std::optional<py::iterable> masses) {
            const auto G = heypy::to_number(Gconst);

            std::vector<hey::number> m_vec;
            if (masses) {
                for (auto ms : *masses) {
                    m_vec.push_back(heypy::to_number(ms));
                }
            } else {
                // If masses are not provided, all masses are 1.
                m_vec.resize(static_cast<decltype(m_vec.size())>(n), hey::number{1.});
            }

            return hey::make_nbody_sys(n, kw::Gconst = G, kw::masses = m_vec);
        },
        "n"_a, "Gconst"_a = 1., "masses"_a = py::none{});

    m.def(
        "make_np1body_sys",
        [](std::uint32_t n, py::object Gconst, std::optional<py::iterable> masses) {
            const auto G = heypy::to_number(Gconst);

            std::vector<hey::number> m_vec;
            if (masses) {
                for (auto ms : *masses) {
                    m_vec.push_back(heypy::to_number(ms));
                }
            } else {
                // If masses are not provided, all masses are 1.
                // NOTE: instead of computing n+1 here, do it in two
                // steps to avoid potential overflow issues.
                m_vec.resize(static_cast<decltype(m_vec.size())>(n), hey::number{1.});
                m_vec.emplace_back(1.);
            }

            return hey::make_np1body_sys(n, kw::Gconst = G, kw::masses = m_vec);
        },
        "n"_a, "Gconst"_a = 1., "masses"_a = py::none{});

    m.def(
        "make_nbody_par_sys",
        [](std::uint32_t n, py::object Gconst, std::optional<std::uint32_t> n_massive) {
            const auto G = heypy::to_number(Gconst);

            if (n_massive) {
                return hey::make_nbody_par_sys(n, kw::Gconst = G, kw::n_massive = *n_massive);
            } else {
                return hey::make_nbody_par_sys(n, kw::Gconst = G);
            }
        },
        "n"_a, "Gconst"_a = 1., "n_massive"_a = py::none{});

    // mascon dynamics builder
    m.def(
        "make_mascon_system",
        [](py::object Gconst, py::iterable points, py::iterable masses, py::iterable omega) {
            const auto G = heypy::to_number(Gconst);

            std::vector<std::vector<hey::expression>> points_vec;
            for (auto p : points) {
                std::vector<hey::expression> tmp;
                for (auto el : py::cast<py::iterable>(p)) {
                    tmp.emplace_back(heypy::to_number(el));
                }
                points_vec.emplace_back(tmp);
            }

            std::vector<hey::expression> mass_vec;
            for (auto ms : masses) {
                mass_vec.emplace_back(heypy::to_number(ms));
            }
            std::vector<hey::expression> omega_vec;
            for (auto w : omega) {
                omega_vec.emplace_back(heypy::to_number(w));
            }

            return hey::make_mascon_system(kw::Gconst = G, kw::points = points_vec, kw::masses = mass_vec,
                                           kw::omega = omega_vec);
        },
        "Gconst"_a, "points"_a, "masses"_a, "omega"_a);

    m.def(
        "energy_mascon_system",
        [](py::object Gconst, py::iterable state, py::iterable points, py::iterable masses, py::iterable omega) {
            const auto G = heypy::to_number(Gconst);

            std::vector<hey::expression> state_vec;
            for (auto s : state) {
                state_vec.emplace_back(heypy::to_number(s));
            }

            std::vector<std::vector<hey::expression>> points_vec;
            for (auto p : points) {
                std::vector<hey::expression> tmp;
                for (auto el : py::cast<py::iterable>(p)) {
                    tmp.emplace_back(heypy::to_number(el));
                }
                points_vec.emplace_back(tmp);
            }

            std::vector<hey::expression> mass_vec;
            for (auto ms : masses) {
                mass_vec.emplace_back(heypy::to_number(ms));
            }

            std::vector<hey::expression> omega_vec;
            for (auto w : omega) {
                omega_vec.emplace_back(heypy::to_number(w));
            }

            return hey::energy_mascon_system(kw::Gconst = G, kw::state = state_vec, kw::points = points_vec,
                                             kw::masses = mass_vec, kw::omega = omega_vec);
        },
        "Gconst"_a, "state"_a, "points"_a, "masses"_a, "omega"_a);

    // taylor_outcome enum.
    py::enum_<hey::taylor_outcome>(m, "taylor_outcome", py::arithmetic())
        .value("success", hey::taylor_outcome::success)
        .value("step_limit", hey::taylor_outcome::step_limit)
        .value("time_limit", hey::taylor_outcome::time_limit)
        .value("err_nf_state", hey::taylor_outcome::err_nf_state)
        .value("cb_stop", hey::taylor_outcome::cb_stop);

    // event_direction enum.
    py::enum_<hey::event_direction>(m, "event_direction")
        .value("any", hey::event_direction::any)
        .value("positive", hey::event_direction::positive)
        .value("negative", hey::event_direction::negative);

    // Computation of the jet of derivatives.
    heypy::expose_taylor_add_jet_dbl(m);
    heypy::expose_taylor_add_jet_ldbl(m);

#if defined(HEYOKA_HAVE_REAL128)

    heypy::expose_taylor_add_jet_f128(m);

#endif

    // Compiled functions.
    heypy::expose_add_cfunc_dbl(m);
    heypy::expose_add_cfunc_ldbl(m);

#if defined(HEYOKA_HAVE_REAL128)

    heypy::expose_add_cfunc_f128(m);

#endif

    // Scalar adaptive taylor integrators.
    heypy::expose_taylor_integrator_dbl(m);
    heypy::expose_taylor_integrator_ldbl(m);

#if defined(HEYOKA_HAVE_REAL128)

    heypy::expose_taylor_integrator_f128(m);

#endif

    // Expose the events.
    heypy::expose_taylor_t_event_dbl(m);
    heypy::expose_taylor_t_event_ldbl(m);

#if defined(HEYOKA_HAVE_REAL128)

    heypy::expose_taylor_t_event_f128(m);

#endif

    heypy::expose_taylor_nt_event_dbl(m);
    heypy::expose_taylor_nt_event_ldbl(m);

#if defined(HEYOKA_HAVE_REAL128)

    heypy::expose_taylor_nt_event_f128(m);

#endif

    // Batch mode.
    heypy::expose_taylor_nt_event_batch_dbl(m);
    heypy::expose_taylor_t_event_batch_dbl(m);

    // LLVM state.
    py::class_<hey::llvm_state>(m, "llvm_state", py::dynamic_attr{})
        .def("get_ir", &hey::llvm_state::get_ir)
        .def("get_object_code", [](hey::llvm_state &s) { return py::bytes(s.get_object_code()); })
        // Repr.
        .def("__repr__",
             [](const hey::llvm_state &s) {
                 std::ostringstream oss;
                 oss << s;
                 return oss.str();
             })
        // Copy/deepcopy.
        .def("__copy__", heypy::copy_wrapper<hey::llvm_state>)
        .def("__deepcopy__", heypy::deepcopy_wrapper<hey::llvm_state>, "memo"_a)
        // Pickle support.
        .def(py::pickle(&heypy::pickle_getstate_wrapper<hey::llvm_state>,
                        &heypy::pickle_setstate_wrapper<hey::llvm_state>));

    // Recommended simd size helper.
    m.def("_recommended_simd_size_dbl", &hey::recommended_simd_size<double>);

    // The callback for the propagate_*() functions for
    // the batch integrator.
    using prop_cb_t = std::function<bool(hey::taylor_adaptive_batch<double> &)>;

    // Event types for the batch integrator.
    using t_ev_t = hey::t_event_batch<double>;
    using nt_ev_t = hey::nt_event_batch<double>;

    // Batch adaptive integrator for double.
    auto tabd_ctor_impl
        = [](const auto &sys, py::array_t<double> state_, std::optional<py::array_t<double>> time_,
             std::optional<py::array_t<double>> pars_, double tol, bool high_accuracy, bool compact_mode,
             std::vector<t_ev_t> tes, std::vector<nt_ev_t> ntes, bool parallel_mode) {
              // Convert state and pars to std::vector, after checking
              // dimensions and shape.
              if (state_.ndim() != 2) {
                  heypy::py_throw(
                      PyExc_ValueError,
                      fmt::format("Invalid state vector passed to the constructor of a batch integrator: "
                                  "the expected number of dimensions is 2, but the input array has a dimension of {}",
                                  state_.ndim())
                          .c_str());
              }

              // Infer the batch size from the second dimension.
              const auto batch_size = boost::numeric_cast<std::uint32_t>(state_.shape(1));

              // Flatten out and convert to a C++ vector.
              auto state = py::cast<std::vector<double>>(state_.attr("flatten")());

              // If pars is none, an empty vector will be fine.
              std::vector<double> pars;
              if (pars_) {
                  auto &pars_arr = *pars_;

                  if (pars_arr.ndim() != 2 || boost::numeric_cast<std::uint32_t>(pars_arr.shape(1)) != batch_size) {
                      heypy::py_throw(
                          PyExc_ValueError,
                          fmt::format("Invalid parameter vector passed to the constructor of a batch integrator: "
                                      "the expected array shape is (n, {}), but the input array has either the wrong "
                                      "number of dimensions or the wrong shape",
                                      batch_size)
                              .c_str());
                  }
                  pars = py::cast<std::vector<double>>(pars_arr.attr("flatten")());
              }

              if (time_) {
                  // Times provided.
                  auto &time_arr = *time_;
                  if (time_arr.ndim() != 1 || boost::numeric_cast<std::uint32_t>(time_arr.shape(0)) != batch_size) {
                      heypy::py_throw(
                          PyExc_ValueError,
                          fmt::format("Invalid time vector passed to the constructor of a batch integrator: "
                                      "the expected array shape is ({}), but the input array has either the wrong "
                                      "number of dimensions or the wrong shape",
                                      batch_size)
                              .c_str());
                  }
                  auto time = py::cast<std::vector<double>>(time_arr);

                  // NOTE: GIL release is fine here even if the events contain
                  // Python objects, as the event vectors are moved in
                  // upon construction and thus we should never end up calling
                  // into the interpreter.
                  py::gil_scoped_release release;

                  return hey::taylor_adaptive_batch<double>{sys,
                                                            std::move(state),
                                                            batch_size,
                                                            kw::time = std::move(time),
                                                            kw::tol = tol,
                                                            kw::high_accuracy = high_accuracy,
                                                            kw::compact_mode = compact_mode,
                                                            kw::pars = std::move(pars),
                                                            kw::t_events = std::move(tes),
                                                            kw::nt_events = std::move(ntes),
                                                            kw::parallel_mode = parallel_mode};
              } else {
                  // Times not provided.

                  // NOTE: GIL release is fine here even if the events contain
                  // Python objects, as the event vectors are moved in
                  // upon construction and thus we should never end up calling
                  // into the interpreter.
                  py::gil_scoped_release release;

                  return hey::taylor_adaptive_batch<double>{sys,
                                                            std::move(state),
                                                            batch_size,
                                                            kw::tol = tol,
                                                            kw::high_accuracy = high_accuracy,
                                                            kw::compact_mode = compact_mode,
                                                            kw::pars = std::move(pars),
                                                            kw::t_events = std::move(tes),
                                                            kw::nt_events = std::move(ntes),
                                                            kw::parallel_mode = parallel_mode};
              }
          };

    py::class_<hey::taylor_adaptive_batch<double>> tabd_c(m, "_taylor_adaptive_batch_dbl", py::dynamic_attr{});
    tabd_c
        .def(py::init([tabd_ctor_impl](const std::vector<std::pair<hey::expression, hey::expression>> &sys,
                                       py::array_t<double> state, std::optional<py::array_t<double>> time,
                                       std::optional<py::array_t<double>> pars, double tol, bool high_accuracy,
                                       bool compact_mode, std::vector<t_ev_t> tes, std::vector<nt_ev_t> ntes,
                                       bool parallel_mode) {
                 return tabd_ctor_impl(sys, state, std::move(time), std::move(pars), tol, high_accuracy, compact_mode,
                                       std::move(tes), std::move(ntes), parallel_mode);
             }),
             "sys"_a, "state"_a, "time"_a = py::none{}, "pars"_a = py::none{}, "tol"_a = 0., "high_accuracy"_a = false,
             "compact_mode"_a = false, "t_events"_a = py::list{}, "nt_events"_a = py::list{}, "parallel_mode"_a = false)
        .def(py::init([tabd_ctor_impl](const std::vector<hey::expression> &sys, py::array_t<double> state,
                                       std::optional<py::array_t<double>> time, std::optional<py::array_t<double>> pars,
                                       double tol, bool high_accuracy, bool compact_mode, std::vector<t_ev_t> tes,
                                       std::vector<nt_ev_t> ntes, bool parallel_mode) {
                 return tabd_ctor_impl(sys, state, std::move(time), std::move(pars), tol, high_accuracy, compact_mode,
                                       std::move(tes), std::move(ntes), parallel_mode);
             }),
             "sys"_a, "state"_a, "time"_a = py::none{}, "pars"_a = py::none{}, "tol"_a = 0., "high_accuracy"_a = false,
             "compact_mode"_a = false, "t_events"_a = py::list{}, "nt_events"_a = py::list{}, "parallel_mode"_a = false)
        .def_property_readonly("decomposition", &hey::taylor_adaptive_batch<double>::get_decomposition)
        .def(
            "step", [](hey::taylor_adaptive_batch<double> &ta, bool wtc) { ta.step(wtc); }, "write_tc"_a = false)
        .def(
            "step",
            [](hey::taylor_adaptive_batch<double> &ta, const std::vector<double> &max_delta_t, bool wtc) {
                ta.step(max_delta_t, wtc);
            },
            "max_delta_t"_a, "write_tc"_a = false)
        .def("step_backward", &hey::taylor_adaptive_batch<double>::step_backward, "write_tc"_a = false)
        .def_property_readonly("step_res",
                               [](const hey::taylor_adaptive_batch<double> &ta) { return ta.get_step_res(); })
        .def(
            "propagate_for",
            [](hey::taylor_adaptive_batch<double> &ta, const std::variant<double, std::vector<double>> &delta_t,
               std::size_t max_steps, std::variant<double, std::vector<double>> max_delta_t, const prop_cb_t &cb_,
               bool write_tc, bool c_output) {
                return std::visit(
                    [&](const auto &dt, auto max_dts) {
                        // Create the callback wrapper.
                        auto cb = heypy::make_prop_cb(cb_);

                        // NOTE: after releasing the GIL here, the only potential
                        // calls into the Python interpreter are when invoking cb
                        // or the events' callbacks (which are all protected by GIL reacquire).
                        // Note that copying cb around or destroying it is harmless, as it contains only
                        // a reference to the original callback cb_, or it is an empty callback.
                        py::gil_scoped_release release;
                        return ta.propagate_for(dt, kw::max_steps = max_steps, kw::max_delta_t = std::move(max_dts),
                                                kw::callback = cb, kw::write_tc = write_tc, kw::c_output = c_output);
                    },
                    delta_t, std::move(max_delta_t));
            },
            "delta_t"_a, "max_steps"_a = 0, "max_delta_t"_a = std::vector<double>{}, "callback"_a = prop_cb_t{},
            "write_tc"_a = false, "c_output"_a = false)
        .def(
            "propagate_until",
            [](hey::taylor_adaptive_batch<double> &ta, const std::variant<double, std::vector<double>> &tm,
               std::size_t max_steps, std::variant<double, std::vector<double>> max_delta_t, const prop_cb_t &cb_,
               bool write_tc, bool c_output) {
                return std::visit(
                    [&](const auto &t, auto max_dts) {
                        // Create the callback wrapper.
                        auto cb = heypy::make_prop_cb(cb_);

                        py::gil_scoped_release release;
                        return ta.propagate_until(t, kw::max_steps = max_steps, kw::max_delta_t = std::move(max_dts),
                                                  kw::callback = cb, kw::write_tc = write_tc, kw::c_output = c_output);
                    },
                    tm, std::move(max_delta_t));
            },
            "t"_a, "max_steps"_a = 0, "max_delta_t"_a = std::vector<double>{}, "callback"_a = prop_cb_t{},
            "write_tc"_a = false, "c_output"_a = false)
        .def(
            "propagate_grid",
            [](hey::taylor_adaptive_batch<double> &ta, py::array_t<double> grid, std::size_t max_steps,
               std::variant<double, std::vector<double>> max_delta_t, const prop_cb_t &cb_) {
                return std::visit(
                    [&](auto max_dts) {
                        // Check the grid dimension/shape.
                        if (grid.ndim() != 2) {
                            heypy::py_throw(
                                PyExc_ValueError,
                                fmt::format(
                                    "Invalid grid passed to the propagate_grid() method of a batch integrator: "
                                    "the expected number of dimensions is 2, but the input array has a dimension of {}",
                                    grid.ndim())
                                    .c_str());
                        }
                        if (boost::numeric_cast<std::uint32_t>(grid.shape(1)) != ta.get_batch_size()) {
                            heypy::py_throw(
                                PyExc_ValueError,
                                fmt::format("Invalid grid passed to the propagate_grid() method of a batch integrator: "
                                            "the shape must be (n, {}) but the number of columns is {} instead",
                                            ta.get_batch_size(), grid.shape(1))
                                    .c_str());
                        }

                        // Convert to a std::vector.
                        const auto grid_v = py::cast<std::vector<double>>(grid.attr("flatten")());

#if !defined(NDEBUG)
                        // Store the grid size for debug.
                        const auto grid_v_size = grid_v.size();
#endif

                        // Create the callback wrapper.
                        auto cb = heypy::make_prop_cb(cb_);

                        // Run the propagation.
                        // NOTE: for batch integrators, ret is guaranteed to always have
                        // the same size regardless of errors.
                        decltype(ta.propagate_grid(grid_v, max_steps)) ret;
                        {
                            py::gil_scoped_release release;
                            ret = ta.propagate_grid(std::move(grid_v), kw::max_steps = max_steps,
                                                    kw::max_delta_t = std::move(max_dts), kw::callback = cb);
                        }

                        // Create the output array.
                        assert(ret.size() == grid_v_size * ta.get_dim());
                        py::array_t<double> a_ret(
                            py::array::ShapeContainer{grid.shape(0), boost::numeric_cast<py::ssize_t>(ta.get_dim()),
                                                      grid.shape(1)},
                            ret.data());

                        return a_ret;
                    },
                    std::move(max_delta_t));
            },
            "grid"_a, "max_steps"_a = 0, "max_delta_t"_a = std::vector<double>{}, "callback"_a = prop_cb_t{})
        .def_property_readonly("propagate_res",
                               [](const hey::taylor_adaptive_batch<double> &ta) { return ta.get_propagate_res(); })
        .def_property_readonly("time",
                               [](py::object &o) {
                                   auto *ta = py::cast<hey::taylor_adaptive_batch<double> *>(o);
                                   py::array_t<double> ret(py::array::ShapeContainer{boost::numeric_cast<py::ssize_t>(
                                                               ta->get_time().size())},
                                                           ta->get_time_data(), o);

                                   // Ensure the returned array is read-only.
                                   ret.attr("flags").attr("writeable") = false;

                                   return ret;
                               })
        .def_property_readonly(
            "dtime",
            [](py::object &o) {
                auto *ta = py::cast<hey::taylor_adaptive_batch<double> *>(o);

                py::array_t<double> hi_ret(
                    py::array::ShapeContainer{boost::numeric_cast<py::ssize_t>(ta->get_dtime().first.size())},
                    ta->get_dtime_data().first, o);
                py::array_t<double> lo_ret(
                    py::array::ShapeContainer{boost::numeric_cast<py::ssize_t>(ta->get_dtime().second.size())},
                    ta->get_dtime_data().second, o);

                // Ensure the returned arrays are read-only.
                hi_ret.attr("flags").attr("writeable") = false;
                lo_ret.attr("flags").attr("writeable") = false;

                return py::make_tuple(hi_ret, lo_ret);
            })
        .def("set_time",
             [](hey::taylor_adaptive_batch<double> &ta, const std::variant<double, std::vector<double>> &tm) {
                 std::visit([&ta](const auto &t) { ta.set_time(t); }, tm);
             })
        .def("set_dtime",
             [](hey::taylor_adaptive_batch<double> &ta, const std::variant<double, std::vector<double>> &hi_tm,
                const std::variant<double, std::vector<double>> &lo_tm) {
                 std::visit(
                     [&ta](const auto &t_hi, const auto &t_lo) {
                         if constexpr (std::is_same_v<decltype(t_hi), decltype(t_lo)>) {
                             ta.set_dtime(t_hi, t_lo);
                         } else {
                             heypy::py_throw(PyExc_TypeError,
                                             "The two arguments to the set_dtime() method must be of the same type");
                         }
                     },
                     hi_tm, lo_tm);
             })
        .def_property_readonly(
            "state",
            [](py::object &o) {
                auto *ta = py::cast<hey::taylor_adaptive_batch<double> *>(o);

                assert(ta->get_state().size() % ta->get_batch_size() == 0u);
                const auto nvars = boost::numeric_cast<py::ssize_t>(ta->get_dim());
                const auto bs = boost::numeric_cast<py::ssize_t>(ta->get_batch_size());

                return py::array_t<double>(py::array::ShapeContainer{nvars, bs}, ta->get_state_data(), o);
            })
        .def_property_readonly(
            "pars",
            [](py::object &o) {
                auto *ta = py::cast<hey::taylor_adaptive_batch<double> *>(o);

                assert(ta->get_pars().size() % ta->get_batch_size() == 0u);
                const auto npars = boost::numeric_cast<py::ssize_t>(ta->get_pars().size() / ta->get_batch_size());
                const auto bs = boost::numeric_cast<py::ssize_t>(ta->get_batch_size());

                return py::array_t<double>(py::array::ShapeContainer{npars, bs}, ta->get_pars_data(), o);
            })
        .def_property_readonly(
            "tc",
            [](const py::object &o) {
                auto *ta = py::cast<const hey::taylor_adaptive_batch<double> *>(o);

                const auto nvars = boost::numeric_cast<py::ssize_t>(ta->get_dim());
                const auto ncoeff = boost::numeric_cast<py::ssize_t>(ta->get_order() + 1u);
                const auto bs = boost::numeric_cast<py::ssize_t>(ta->get_batch_size());

                auto ret = py::array_t<double>(py::array::ShapeContainer{nvars, ncoeff, bs}, ta->get_tc().data(), o);

                // Ensure the returned array is read-only.
                ret.attr("flags").attr("writeable") = false;

                return ret;
            })
        .def_property_readonly(
            "last_h",
            [](const py::object &o) {
                auto *ta = py::cast<const hey::taylor_adaptive_batch<double> *>(o);

                auto ret = py::array_t<double>(
                    py::array::ShapeContainer{boost::numeric_cast<py::ssize_t>(ta->get_batch_size())},
                    ta->get_last_h().data(), o);

                // Ensure the returned array is read-only.
                ret.attr("flags").attr("writeable") = false;

                return ret;
            })
        .def_property_readonly(
            "d_output",
            [](const py::object &o) {
                auto *ta = py::cast<const hey::taylor_adaptive_batch<double> *>(o);

                const auto nvars = boost::numeric_cast<py::ssize_t>(ta->get_dim());
                const auto bs = boost::numeric_cast<py::ssize_t>(ta->get_batch_size());

                auto ret = py::array_t<double>(py::array::ShapeContainer{nvars, bs}, ta->get_d_output().data(), o);

                // Ensure the returned array is read-only.
                ret.attr("flags").attr("writeable") = false;

                return ret;
            })
        .def(
            "update_d_output",
            [](py::object &o, const std::variant<double, std::vector<double>> &tm, bool rel_time) {
                return std::visit(
                    [&o, rel_time](const auto &t) {
                        auto *ta = py::cast<hey::taylor_adaptive_batch<double> *>(o);

                        ta->update_d_output(t, rel_time);

                        const auto nvars = boost::numeric_cast<py::ssize_t>(ta->get_dim());
                        const auto bs = boost::numeric_cast<py::ssize_t>(ta->get_batch_size());

                        auto ret
                            = py::array_t<double>(py::array::ShapeContainer{nvars, bs}, ta->get_d_output().data(), o);

                        // Ensure the returned array is read-only.
                        ret.attr("flags").attr("writeable") = false;

                        return ret;
                    },
                    tm);
            },
            "t"_a, "rel_time"_a = false)
        .def_property_readonly("order", &hey::taylor_adaptive_batch<double>::get_order)
        .def_property_readonly("tol", &hey::taylor_adaptive_batch<double>::get_tol)
        .def_property_readonly("dim", &hey::taylor_adaptive_batch<double>::get_dim)
        .def_property_readonly("batch_size", &hey::taylor_adaptive_batch<double>::get_batch_size)
        .def_property_readonly("compact_mode", &hey::taylor_adaptive_batch<double>::get_compact_mode)
        .def_property_readonly("high_accuracy", &hey::taylor_adaptive_batch<double>::get_high_accuracy)
        .def_property_readonly("with_events", &hey::taylor_adaptive_batch<double>::with_events)
        // Event detection.
        .def_property_readonly("with_events", &hey::taylor_adaptive_batch<double>::with_events)
        .def_property_readonly("te_cooldowns", &hey::taylor_adaptive_batch<double>::get_te_cooldowns)
        .def("reset_cooldowns", [](hey::taylor_adaptive_batch<double> &ta) { ta.reset_cooldowns(); })
        .def("reset_cooldowns", [](hey::taylor_adaptive_batch<double> &ta, std::uint32_t i) { ta.reset_cooldowns(i); })
        .def_property_readonly("t_events", &hey::taylor_adaptive_batch<double>::get_t_events)
        .def_property_readonly("nt_events", &hey::taylor_adaptive_batch<double>::get_nt_events)
        // Repr.
        .def("__repr__",
             [](const hey::taylor_adaptive_batch<double> &ta) {
                 std::ostringstream oss;
                 oss << ta;
                 return oss.str();
             })
        // Copy/deepcopy.
        .def("__copy__", heypy::copy_wrapper<hey::taylor_adaptive_batch<double>>)
        .def("__deepcopy__", heypy::deepcopy_wrapper<hey::taylor_adaptive_batch<double>>, "memo"_a)
        // Pickle support.
        .def(py::pickle(&heypy::pickle_getstate_wrapper<hey::taylor_adaptive_batch<double>>,
                        &heypy::pickle_setstate_wrapper<hey::taylor_adaptive_batch<double>>));

    // Expose the llvm state getter.
    heypy::expose_llvm_state_property(tabd_c);

    // Setup the sympy integration bits.
    heypy::setup_sympy(m);

    // Expose the vsop2013 functions.
    m.def(
        "vsop2013_elliptic",
        [](std::uint32_t pl_idx, std::uint32_t var_idx, hey::expression t_expr, double thresh) {
            return hey::vsop2013_elliptic(pl_idx, var_idx, kw::time = std::move(t_expr), kw::thresh = thresh);
        },
        "pl_idx"_a, "var_idx"_a = 0, "time"_a = hey::time, "thresh"_a = 1e-9);
    m.def(
        "vsop2013_cartesian",
        [](std::uint32_t pl_idx, hey::expression t_expr, double thresh) {
            return hey::vsop2013_cartesian(pl_idx, kw::time = std::move(t_expr), kw::thresh = thresh);
        },
        "pl_idx"_a, "time"_a = hey::time, "thresh"_a = 1e-9);
    m.def(
        "vsop2013_cartesian_icrf",
        [](std::uint32_t pl_idx, hey::expression t_expr, double thresh) {
            return hey::vsop2013_cartesian_icrf(pl_idx, kw::time = std::move(t_expr), kw::thresh = thresh);
        },
        "pl_idx"_a, "time"_a = hey::time, "thresh"_a = 1e-9);
    m.def("get_vsop2013_mus", &hey::get_vsop2013_mus);

    // Expose the continuous output function objects.
    heypy::taylor_expose_c_output(m);

    // Expose the helpers to get/set the number of threads in use by heyoka.py.
    m.def("set_nthreads", [](std::size_t n) {
        if (n == 0u) {
            heypy::detail::tbb_gc.reset();
        } else {
            heypy::detail::tbb_gc.emplace(oneapi::tbb::global_control::max_allowed_parallelism, n);
        }
    });

    m.def("get_nthreads", []() {
        return oneapi::tbb::global_control::active_value(oneapi::tbb::global_control::max_allowed_parallelism);
    });

    // Make sure the TBB control structure is cleaned
    // up before shutdown.
    auto atexit = py::module_::import("atexit");
    atexit.attr("register")(py::cpp_function([]() {
#if !defined(NDEBUG)
        std::cout << "Cleaning up the TBB control structure" << std::endl;
#endif
        heypy::detail::tbb_gc.reset();
    }));
}

#if defined(__clang__)

#pragma clang diagnostic pop

#endif
