// sqlite3ppext.h
//
// The MIT License
//
// Copyright (c) 2015 Wongoo Lee (iwongu at gmail dot com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef SQLITE3PPEXT_H
#define SQLITE3PPEXT_H

#include <cstddef>
#include <EASTL/map.h>
#include <EASTL/tuple.h>
#include <EASTL/type_traits.h>
#include <EASTL/utility.h>
#include <EASTL/shared_ptr.h>

#include "sqlite3pp.h"

namespace sqlite3pp
{
	namespace
	{
		template<size_t N>
		struct Apply
		{
			template<typename F, typename T, typename... A>
			static inline auto apply(F&& f, T&& t, A&&... a)
				-> decltype(Apply<N - 1>::apply(eastl::forward<F>(f),
					eastl::forward<T>(t),
					eastl::get<N - 1>(eastl::forward<T>(t)),
					eastl::forward<A>(a)...))
			{
				return Apply<N - 1>::apply(eastl::forward<F>(f),
					eastl::forward<T>(t),
					eastl::get<N - 1>(eastl::forward<T>(t)),
					eastl::forward<A>(a)...);
			}
		};

		template<>
		struct Apply<0>
		{
			template<typename F, typename T, typename... A>
			static inline auto apply(F&& f, T&&, A&&... a)
				-> decltype(eastl::forward<F>(f)(eastl::forward<A>(a)...))
			{
				return eastl::forward<F>(f)(eastl::forward<A>(a)...);
			}
		};

		template<typename F, typename T>
		inline auto apply_f(F&& f, T&& t)
			-> decltype(Apply<eastl::tuple_size<typename eastl::decay<T>::type>::value>::apply(eastl::forward<F>(f), eastl::forward<T>(t)))
		{
			return Apply<eastl::tuple_size<typename eastl::decay<T>::type>::value>::apply(
				eastl::forward<F>(f), eastl::forward<T>(t));
		}
	}


	namespace ext
	{
		database borrow(sqlite3* pdb)
		{
			return database(pdb);
		}

		class context : noncopyable
		{
		public:
			explicit context(sqlite3_context* ctx, int nargs = 0, sqlite3_value** values = nullptr);

			int args_count() const;
			int args_bytes(int idx) const;
			int args_type(int idx) const;

			template <class T> T get(int idx) const
			{
				return get(idx, T());
			}

			void result(int value);
			void result(double value);
			void result(long long int value);
			void result(eastl::string const& value);
			void result(char const* value, bool fcopy);
			void result(void const* value, int n, bool fcopy);
			void result();
			void result(null_type);
			void result_copy(int idx);
			void result_error(char const* msg);

			void* aggregate_data(int size);
			int aggregate_count();

			template <class... Ts>
			eastl::tuple<Ts...> to_tuple()
			{
				return to_tuple_impl(0, *this, eastl::tuple<Ts...>());
			}

		private:
			int get(int idx, int) const;
			double get(int idx, double) const;
			long long int get(int idx, long long int) const;
			char const* get(int idx, char const*) const;
			eastl::string get(int idx, eastl::string) const;
			void const* get(int idx, void const*) const;

			template<class H, class... Ts>
			static inline eastl::tuple<H, Ts...> to_tuple_impl(int index, const context& c, eastl::tuple<H, Ts...>&&)
			{
				return eastl::tuple_cat(eastl::make_tuple(c.context::get<H>(index)), to_tuple_impl(++index, c, eastl::tuple<Ts...>()));
			}
			static inline eastl::tuple<> to_tuple_impl(int /*index*/, const context& /*c*/, eastl::tuple<>&&)
			{
				return eastl::tuple<>();
			}

		private:
			sqlite3_context* ctx_;
			int nargs_;
			sqlite3_value** values_;
		};

		namespace
		{
			template <class R, class... Ps>
			void functionx_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
			{
				context c(ctx, nargs, values);
				auto f = static_cast<eastl::function<R(Ps...)>*>(sqlite3_user_data(ctx));
				c.result(apply_f(*f, c.to_tuple<Ps...>()));
			}
		}

		class function : noncopyable
		{
		public:
			using function_handler = eastl::function<void(context&)>;
			using pfunction_base = eastl::shared_ptr<void>;

			explicit function(database& db);

			int create(char const* name, function_handler h, int nargs = 0);

			template <class F> int create(char const* name, eastl::function<F> h)
			{
				fh_[name] = eastl::shared_ptr<void>(new eastl::function<F>(h));
				return create_function_impl<F>()(db_, fh_[name].get(), name);
			}

		private:

			template<class R, class... Ps>
			struct create_function_impl;

			template<class R, class... Ps>
			struct create_function_impl<R(Ps...)>
			{
				int operator()(sqlite3* db, void* fh, char const* name)
				{
					return sqlite3_create_function(db, name, sizeof...(Ps), SQLITE_UTF8, fh,
						functionx_impl<R, Ps...>,
						0, 0);
				}
			};

		private:
			sqlite3* db_;

			eastl::map<eastl::string, pfunction_base> fh_;
		};

		namespace
		{
			template <class T, class... Ps>
			void stepx_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
			{
				context c(ctx, nargs, values);
				T* t = static_cast<T*>(c.aggregate_data(sizeof(T)));
				if (c.aggregate_count() == 1) new (t) T;
				apply_f([](T* tt, Ps... ps) { tt->step(ps...); }, eastl::tuple_cat(eastl::make_tuple(t), c.to_tuple<Ps...>()));
			}

			template <class T>
			void finishN_impl(sqlite3_context* ctx)
			{
				context c(ctx);
				T* t = static_cast<T*>(c.aggregate_data(sizeof(T)));
				c.result(t->finish());
				t->~T();
			}
		}

		class aggregate : noncopyable
		{
		public:
			using function_handler = eastl::function<void(context&)>;
			using pfunction_base = eastl::shared_ptr<void>;

			explicit aggregate(database& db);

			int create(char const* name, function_handler s, function_handler f, int nargs = 1);

			template <class T, class... Ps>
			int create(char const* name)
			{
				return sqlite3_create_function(db_, name, sizeof...(Ps), SQLITE_UTF8, 0, 0, stepx_impl<T, Ps...>, finishN_impl<T>);
			}

		private:
			sqlite3* db_;

			eastl::map<eastl::string, eastl::pair<pfunction_base, pfunction_base> > ah_;
		};

	} // namespace ext

} // namespace sqlite3pp

#include "sqlite3ppext.ipp"

#endif
