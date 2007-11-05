//
//  Copyright (c) 2000-2002
//  Joerg Walter, Mathias Koch
//
//  Permission to use, copy, modify, distribute and sell this software
//  and its documentation for any purpose is hereby granted without fee,
//  provided that the above copyright notice appear in all copies and
//  that both that copyright notice and this permission notice appear
//  in supporting documentation.  The authors make no representations
//  about the suitability of this software for any purpose.
//  It is provided "as is" without express or implied warranty.
//
//  The authors gratefully acknowledge the support of
//  GeNeSys mbH & Co. KG in producing this work.
//

#ifndef _BOOST_UBLAS_VECTOR_
#define _BOOST_UBLAS_VECTOR_

#include <boost/numeric/ublas/storage.hpp>
#include <boost/numeric/ublas/vector_expression.hpp>
#include <boost/numeric/ublas/detail/vector_assign.hpp>

// Iterators based on ideas of Jeremy Siek

namespace boost { namespace numeric { namespace ublas {

    // Array based vector class
    template<class T, class A>
    class vector:
        public vector_container<vector<T, A> > {

        typedef vector<T, A> self_type;
    public:
#ifdef BOOST_UBLAS_ENABLE_PROXY_SHORTCUTS
        using vector_container<self_type>::operator ();
#endif
        typedef typename A::size_type size_type;
        typedef typename A::difference_type difference_type;
        typedef T value_type;
        typedef typename type_traits<T>::const_reference const_reference;
        typedef T &reference;
        typedef T *pointer;
        typedef const T *const_pointer;
        typedef A array_type;
        typedef const vector_reference<const self_type> const_closure_type;
        typedef vector_reference<self_type> closure_type;
        typedef self_type vector_temporary_type;
        typedef dense_tag storage_category;

        // Construction and destruction
        BOOST_UBLAS_INLINE
        vector ():
            vector_container<self_type> (),
            data_ () {}
        explicit BOOST_UBLAS_INLINE
        vector (size_type size):
            vector_container<self_type> (),
            data_ (size) {
        }
        BOOST_UBLAS_INLINE
        vector (size_type size, const array_type &data):
            vector_container<self_type> (),
            data_ (data) {}
        BOOST_UBLAS_INLINE
        vector (const vector &v):
            vector_container<self_type> (),
            data_ (v.data_) {}
        template<class AE>
        BOOST_UBLAS_INLINE
        vector (const vector_expression<AE> &ae):
            vector_container<self_type> (),
            data_ (ae ().size ()) {
            vector_assign<scalar_assign> (*this, ae);
        }

        // Random Access Container
        BOOST_UBLAS_INLINE
        size_type max_size () const {
            return data_.max_size ();
        }
        
        BOOST_UBLAS_INLINE
        bool empty () const {
            return data_.size () == 0;
        }
        // Accessors
        BOOST_UBLAS_INLINE
        size_type size () const {
            return data_.size ();
        }

        // Storage accessors
        BOOST_UBLAS_INLINE
        const array_type &data () const {
            return data_;
        }
        BOOST_UBLAS_INLINE
        array_type &data () {
            return data_;
        }

        // Resizing
        BOOST_UBLAS_INLINE
        void resize (size_type size, bool preserve = true) {
            if (preserve)
                data ().resize (size, typename A::value_type ());
            else
                data ().resize (size);
        }

        // Element support
        BOOST_UBLAS_INLINE
        pointer find_element (size_type i) {
            return const_cast<pointer> (const_cast<const self_type&>(*this).find_element (i));
        }
        BOOST_UBLAS_INLINE
        const_pointer find_element (size_type i) const {
            return & (data () [i]);
        }

        // Element access
        BOOST_UBLAS_INLINE
        const_reference operator () (size_type i) const {
            return data () [i];
        }
        BOOST_UBLAS_INLINE
        reference operator () (size_type i) {
            return data () [i];
        }

        BOOST_UBLAS_INLINE
        const_reference operator [] (size_type i) const {
            return (*this) (i);
        }
        BOOST_UBLAS_INLINE
        reference operator [] (size_type i) {
            return (*this) (i);
        }

        // Element assignment
        BOOST_UBLAS_INLINE
        reference insert_element (size_type i, const_reference t) {
            return (data () [i] = t);
        }
        BOOST_UBLAS_INLINE
        void erase_element (size_type i) {
            data () [i] = value_type/*zero*/();
        }
        
        // Zeroing
        BOOST_UBLAS_INLINE
        void clear () {
            std::fill (data ().begin (), data ().end (), value_type/*zero*/());
        }

        // Assignment
        BOOST_UBLAS_INLINE
        vector &operator = (const vector &v) {
            data () = v.data ();
            return *this;
        }
        template<class C>          // Container assignment without temporary
        BOOST_UBLAS_INLINE
        vector &operator = (const vector_container<C> &v) {
            resize (v ().size (), false);
            assign (v);
            return *this;
        }
        BOOST_UBLAS_INLINE
        vector &assign_temporary (vector &v) {
            swap (v);
            return *this;
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        vector &operator = (const vector_expression<AE> &ae) {
            self_type temporary (ae);
            return assign_temporary (temporary);
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        vector &assign (const vector_expression<AE> &ae) {
            vector_assign<scalar_assign> (*this, ae);
            return *this;
        }

        // Computed assignment
        template<class AE>
        BOOST_UBLAS_INLINE
        vector &operator += (const vector_expression<AE> &ae) {
            self_type temporary (*this + ae);
            return assign_temporary (temporary);
        }
        template<class C>          // Container assignment without temporary
        BOOST_UBLAS_INLINE
        vector &operator += (const vector_container<C> &v) {
            plus_assign (v);
            return *this;
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        vector &plus_assign (const vector_expression<AE> &ae) {
            vector_assign<scalar_plus_assign> (*this, ae);
            return *this;
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        vector &operator -= (const vector_expression<AE> &ae) {
            self_type temporary (*this - ae);
            return assign_temporary (temporary);
        }
        template<class C>          // Container assignment without temporary
        BOOST_UBLAS_INLINE
        vector &operator -= (const vector_container<C> &v) {
            minus_assign (v);
            return *this;
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        vector &minus_assign (const vector_expression<AE> &ae) {
            vector_assign<scalar_minus_assign> (*this, ae);
            return *this;
        }
        template<class AT>
        BOOST_UBLAS_INLINE
        vector &operator *= (const AT &at) {
            vector_assign_scalar<scalar_multiplies_assign> (*this, at);
            return *this;
        }
        template<class AT>
        BOOST_UBLAS_INLINE
        vector &operator /= (const AT &at) {
            vector_assign_scalar<scalar_divides_assign> (*this, at);
            return *this;
        }

        // Swapping
        BOOST_UBLAS_INLINE
        void swap (vector &v) {
            if (this != &v) {
                data ().swap (v.data ());
            }
        }
        BOOST_UBLAS_INLINE
        friend void swap (vector &v1, vector &v2) {
            v1.swap (v2);
        }

        // Iterator types
    private:
        // Use the storage array iterator
        typedef typename A::const_iterator const_subiterator_type;
        typedef typename A::iterator subiterator_type;

    public:
#ifdef BOOST_UBLAS_USE_INDEXED_ITERATOR
        typedef indexed_iterator<self_type, dense_random_access_iterator_tag> iterator;
        typedef indexed_const_iterator<self_type, dense_random_access_iterator_tag> const_iterator;
#else
        class const_iterator;
        class iterator;
#endif

        // Element lookup
        BOOST_UBLAS_INLINE
        const_iterator find (size_type i) const {
#ifndef BOOST_UBLAS_USE_INDEXED_ITERATOR
            return const_iterator (*this, data ().begin () + i);
#else
            return const_iterator (*this, i);
#endif
        }
        BOOST_UBLAS_INLINE
        iterator find (size_type i) {
#ifndef BOOST_UBLAS_USE_INDEXED_ITERATOR
            return iterator (*this, data ().begin () + i);
#else
            return iterator (*this, i);
#endif
        }

#ifndef BOOST_UBLAS_USE_INDEXED_ITERATOR
        class const_iterator:
            public container_const_reference<vector>,
            public random_access_iterator_base<dense_random_access_iterator_tag,
                                               const_iterator, value_type, difference_type> {
        public:
            typedef typename vector::difference_type difference_type;
            typedef typename vector::value_type value_type;
            typedef typename vector::const_reference reference;
            typedef const typename vector::pointer pointer;

            // Construction and destruction
            BOOST_UBLAS_INLINE
            const_iterator ():
                container_const_reference<self_type> (), it_ () {}
            BOOST_UBLAS_INLINE
            const_iterator (const self_type &v, const const_subiterator_type &it):
                container_const_reference<self_type> (v), it_ (it) {}
            BOOST_UBLAS_INLINE
            const_iterator (const typename vector::iterator &it):  // ISSUE vector:: stops VC8 using std::iterator here
                container_const_reference<self_type> (it ()), it_ (it.it_) {}

            // Arithmetic
            BOOST_UBLAS_INLINE
            const_iterator &operator ++ () {
                ++ it_;
                return *this;
            }
            BOOST_UBLAS_INLINE
            const_iterator &operator -- () {
                -- it_;
                return *this;
            }
            BOOST_UBLAS_INLINE
            const_iterator &operator += (difference_type n) {
                it_ += n;
                return *this;
            }
            BOOST_UBLAS_INLINE
            const_iterator &operator -= (difference_type n) {
                it_ -= n;
                return *this;
            }
            BOOST_UBLAS_INLINE
            difference_type operator - (const const_iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ - it.it_;
            }

            // Dereference
            BOOST_UBLAS_INLINE
            const_reference operator * () const {
                BOOST_UBLAS_CHECK (it_ >= (*this) ().begin ().it_ && it_ < (*this) ().end ().it_, bad_index ());
                return *it_;
            }
            BOOST_UBLAS_INLINE
            const_reference operator [] (difference_type n) const {
                return *(it_ + n);
            }

            // Index
            BOOST_UBLAS_INLINE
            size_type index () const {
                BOOST_UBLAS_CHECK (it_ >= (*this) ().begin ().it_ && it_ < (*this) ().end ().it_, bad_index ());
                return it_ - (*this) ().begin ().it_;
            }

            // Assignment
            BOOST_UBLAS_INLINE
            const_iterator &operator = (const const_iterator &it) {
                container_const_reference<self_type>::assign (&it ());
                it_ = it.it_;
                return *this;
            }

            // Comparison
            BOOST_UBLAS_INLINE
            bool operator == (const const_iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ == it.it_;
            }
            BOOST_UBLAS_INLINE
            bool operator < (const const_iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ < it.it_;
            }

        private:
            const_subiterator_type it_;

            friend class iterator;
        };
#endif

        BOOST_UBLAS_INLINE
        const_iterator begin () const {
            return find (0);
        }
        BOOST_UBLAS_INLINE
        const_iterator end () const {
            return find (data_.size ());
        }

#ifndef BOOST_UBLAS_USE_INDEXED_ITERATOR
        class iterator:
            public container_reference<vector>,
            public random_access_iterator_base<dense_random_access_iterator_tag,
                                               iterator, value_type, difference_type> {
        public:
            typedef typename vector::difference_type difference_type;
            typedef typename vector::value_type value_type;
            typedef typename vector::reference reference;
            typedef typename vector::pointer pointer;


            // Construction and destruction
            BOOST_UBLAS_INLINE
            iterator ():
                container_reference<self_type> (), it_ () {}
            BOOST_UBLAS_INLINE
            iterator (self_type &v, const subiterator_type &it):
                container_reference<self_type> (v), it_ (it) {}

            // Arithmetic
            BOOST_UBLAS_INLINE
            iterator &operator ++ () {
                ++ it_;
                return *this;
            }
            BOOST_UBLAS_INLINE
            iterator &operator -- () {
                -- it_;
                return *this;
            }
            BOOST_UBLAS_INLINE
            iterator &operator += (difference_type n) {
                it_ += n;
                return *this;
            }
            BOOST_UBLAS_INLINE
            iterator &operator -= (difference_type n) {
                it_ -= n;
                return *this;
            }
            BOOST_UBLAS_INLINE
            difference_type operator - (const iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ - it.it_;
            }

            // Dereference
            BOOST_UBLAS_INLINE
            reference operator * () const {
                BOOST_UBLAS_CHECK (it_ >= (*this) ().begin ().it_ && it_ < (*this) ().end ().it_ , bad_index ());
                return *it_;
            }
            BOOST_UBLAS_INLINE
            reference operator [] (difference_type n) const {
                return *(it_ + n);
            }

            // Index
            BOOST_UBLAS_INLINE
            size_type index () const {
                BOOST_UBLAS_CHECK (it_ >= (*this) ().begin ().it_ && it_ < (*this) ().end ().it_ , bad_index ());
                return it_ - (*this) ().begin ().it_;
            }

            // Assignment
            BOOST_UBLAS_INLINE
            iterator &operator = (const iterator &it) {
                container_reference<self_type>::assign (&it ());
                it_ = it.it_;
                return *this;
            }

            // Comparison
            BOOST_UBLAS_INLINE
            bool operator == (const iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ == it.it_;
            }
            BOOST_UBLAS_INLINE
            bool operator < (const iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ < it.it_;
            }

        private:
            subiterator_type it_;

            friend class const_iterator;
        };
#endif

        BOOST_UBLAS_INLINE
        iterator begin () {
            return find (0);
        }
        BOOST_UBLAS_INLINE
        iterator end () {
            return find (data_.size ());
        }

        // Reverse iterator
        typedef reverse_iterator_base<const_iterator> const_reverse_iterator;
        typedef reverse_iterator_base<iterator> reverse_iterator;

        BOOST_UBLAS_INLINE
        const_reverse_iterator rbegin () const {
            return const_reverse_iterator (end ());
        }
        BOOST_UBLAS_INLINE
        const_reverse_iterator rend () const {
            return const_reverse_iterator (begin ());
        }
        BOOST_UBLAS_INLINE
        reverse_iterator rbegin () {
            return reverse_iterator (end ());
        }
        BOOST_UBLAS_INLINE
        reverse_iterator rend () {
            return reverse_iterator (begin ());
        }

    private:
        array_type data_;
    };


    // Bounded vector class
    template<class T, std::size_t N>
    class bounded_vector:
        public vector<T, bounded_array<T, N> > {

        typedef vector<T, bounded_array<T, N> > vector_type;
    public:
        typedef typename vector_type::size_type size_type;
        static const size_type max_size = N;

        // Construction and destruction
        BOOST_UBLAS_INLINE
        bounded_vector ():
            vector_type (N) {}
        BOOST_UBLAS_INLINE
        bounded_vector (size_type size):
            vector_type (size) {}
        BOOST_UBLAS_INLINE
        bounded_vector (const bounded_vector &v):
            vector_type (v) {}
        template<class A2>              // Allow vector<T,bounded_array<N> construction
        BOOST_UBLAS_INLINE
        bounded_vector (const vector<T, A2> &v):
            vector_type (v) {}
        template<class AE>
        BOOST_UBLAS_INLINE
        bounded_vector (const vector_expression<AE> &ae):
            vector_type (ae) {}
        BOOST_UBLAS_INLINE
        ~bounded_vector () {}

        // Assignment
        BOOST_UBLAS_INLINE
        bounded_vector &operator = (const bounded_vector &v) {
            vector_type::operator = (v);
            return *this;
        }
        template<class A2>         // Generic vector assignment
        BOOST_UBLAS_INLINE
        bounded_vector &operator = (const vector<T, A2> &v) {
            vector_type::operator = (v);
            return *this;
        }
        template<class C>          // Container assignment without temporary
        BOOST_UBLAS_INLINE
        bounded_vector &operator = (const vector_container<C> &v) {
            vector_type::operator = (v);
            return *this;
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        bounded_vector &operator = (const vector_expression<AE> &ae) {
            vector_type::operator = (ae);
            return *this;
        }
    };


    // Zero vector class
    template<class T>
    class zero_vector:
        public vector_container<zero_vector<T> > {

        typedef const T *const_pointer;
        typedef zero_vector<T> self_type;
    public:
#ifdef BOOST_UBLAS_ENABLE_PROXY_SHORTCUTS
        using vector_container<self_type>::operator ();
#endif
        typedef std::size_t size_type;
        typedef std::ptrdiff_t difference_type;
        typedef T value_type;
        typedef const T &const_reference;
        typedef T &reference;
        typedef const vector_reference<const self_type> const_closure_type;
        typedef vector_reference<self_type> closure_type;
        typedef sparse_tag storage_category;

        // Construction and destruction
        BOOST_UBLAS_INLINE
        zero_vector ():
            vector_container<self_type> (),
            size_ (0) {}
        explicit BOOST_UBLAS_INLINE
        zero_vector (size_type size):
            vector_container<self_type> (),
            size_ (size) {}
        BOOST_UBLAS_INLINE
        zero_vector (const zero_vector &v):
            vector_container<self_type> (),
            size_ (v.size_) {}

        // Accessors
        BOOST_UBLAS_INLINE
        size_type size () const {
            return size_;
        }

        // Resizing
        BOOST_UBLAS_INLINE
        void resize (size_type size, bool /*preserve*/ = true) {
            size_ = size;
        }

        // Element support
        BOOST_UBLAS_INLINE
        const_pointer find_element (size_type i) const {
            return & zero_;
        }

        // Element access
        BOOST_UBLAS_INLINE
        const_reference operator () (size_type /* i */) const {
            return zero_;
        }

        BOOST_UBLAS_INLINE
        const_reference operator [] (size_type i) const {
            return (*this) (i);
        }

        // Assignment
        BOOST_UBLAS_INLINE
        zero_vector &operator = (const zero_vector &v) {
            size_ = v.size_;
            return *this;
        }
        BOOST_UBLAS_INLINE
        zero_vector &assign_temporary (zero_vector &v) {
            swap (v);
            return *this;
        }

        // Swapping
        BOOST_UBLAS_INLINE
        void swap (zero_vector &v) {
            if (this != &v) {
                std::swap (size_, v.size_);
            }
        }
        BOOST_UBLAS_INLINE
        friend void swap (zero_vector &v1, zero_vector &v2) {
            v1.swap (v2);
        }

        // Iterator types
    public:
        class const_iterator;

        // Element lookup
        BOOST_UBLAS_INLINE
        const_iterator find (size_type /*i*/) const {
            return const_iterator (*this);
        }

        class const_iterator:
            public container_const_reference<zero_vector>,
            public bidirectional_iterator_base<sparse_bidirectional_iterator_tag,
                                               const_iterator, value_type> {
        public:
            typedef typename zero_vector::difference_type difference_type;
            typedef typename zero_vector::value_type value_type;
            typedef typename zero_vector::const_reference reference;
            typedef typename zero_vector::const_pointer pointer;

            // Construction and destruction
            BOOST_UBLAS_INLINE
            const_iterator ():
                container_const_reference<self_type> () {}
            BOOST_UBLAS_INLINE
            const_iterator (const self_type &v):
                container_const_reference<self_type> (v) {}

            // Arithmetic
            BOOST_UBLAS_INLINE
            const_iterator &operator ++ () {
                BOOST_UBLAS_CHECK_FALSE (bad_index ());
                return *this;
            }
            BOOST_UBLAS_INLINE
            const_iterator &operator -- () {
                BOOST_UBLAS_CHECK_FALSE (bad_index ());
                return *this;
            }

            // Dereference
            BOOST_UBLAS_INLINE
            const_reference operator * () const {
                BOOST_UBLAS_CHECK_FALSE (bad_index ());
                return zero_;   // arbitary return value
            }

            // Index
            BOOST_UBLAS_INLINE
            size_type index () const {
                BOOST_UBLAS_CHECK_FALSE (bad_index ());
                return 0;   // arbitary return value
            }

            // Assignment
            BOOST_UBLAS_INLINE
            const_iterator &operator = (const const_iterator &it) {
                container_const_reference<self_type>::assign (&it ());
                return *this;
            }

            // Comparison
            BOOST_UBLAS_INLINE
            bool operator == (const const_iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                detail::ignore_unused_variable_warning(it);
                return true;
            }
        };

        typedef const_iterator iterator;

        BOOST_UBLAS_INLINE
        const_iterator begin () const {
            return const_iterator (*this);
        }
        BOOST_UBLAS_INLINE
        const_iterator end () const {
            return const_iterator (*this);
        }

        // Reverse iterator
        typedef reverse_iterator_base<const_iterator> const_reverse_iterator;

        BOOST_UBLAS_INLINE
        const_reverse_iterator rbegin () const {
            return const_reverse_iterator (end ());
        }
        BOOST_UBLAS_INLINE
        const_reverse_iterator rend () const {
            return const_reverse_iterator (begin ());
        }

    private:
        size_type size_;
        typedef const value_type const_value_type;
        static const_value_type zero_;
    };

    template<class T>
    typename zero_vector<T>::const_value_type zero_vector<T>::zero_ (0);


    // Unit vector class
    template<class T>
    class unit_vector:
        public vector_container<unit_vector<T> > {

        typedef const T *const_pointer;
        typedef unit_vector<T> self_type;
    public:
#ifdef BOOST_UBLAS_ENABLE_PROXY_SHORTCUTS
        using vector_container<self_type>::operator ();
#endif
        typedef std::size_t size_type;
        typedef std::ptrdiff_t difference_type;
        typedef T value_type;
        typedef const T &const_reference;
        typedef T &reference;
        typedef const vector_reference<const self_type> const_closure_type;
        typedef vector_reference<self_type> closure_type;
        typedef sparse_tag storage_category;

        // Construction and destruction
        BOOST_UBLAS_INLINE
        unit_vector ():
            vector_container<self_type> (),
            size_ (0), index_ (0) {}
        BOOST_UBLAS_INLINE
        explicit unit_vector (size_type size, size_type index = 0):
            vector_container<self_type> (),
            size_ (size), index_ (index) {}
        BOOST_UBLAS_INLINE
        unit_vector (const unit_vector &v):
            vector_container<self_type> (),
            size_ (v.size_), index_ (v.index_) {}

        // Accessors
        BOOST_UBLAS_INLINE
        size_type size () const {
            return size_;
        }
        BOOST_UBLAS_INLINE
        size_type index () const {
            return index_;
        }

        // Resizing
        BOOST_UBLAS_INLINE
        void resize (size_type size, bool /*preserve*/ = true) {
            size_ = size;
        }

        // Element support
        BOOST_UBLAS_INLINE
        const_pointer find_element (size_type i) const {
            if (i == index_)
                return & one_;
            else
                return & zero_;
        }

        // Element access
        BOOST_UBLAS_INLINE
        const_reference operator () (size_type i) const {
            if (i == index_)
                return one_;
            else
                return zero_;
        }

        BOOST_UBLAS_INLINE
        const_reference operator [] (size_type i) const {
            return (*this) (i);
        }

        // Assignment
        BOOST_UBLAS_INLINE
        unit_vector &operator = (const unit_vector &v) {
            size_ = v.size_;
            index_ = v.index_;
            return *this;
        }
        BOOST_UBLAS_INLINE
        unit_vector &assign_temporary (unit_vector &v) {
            swap (v);
            return *this;
        }

        // Swapping
        BOOST_UBLAS_INLINE
        void swap (unit_vector &v) {
            if (this != &v) {
                std::swap (size_, v.size_);
                std::swap (index_, v.index_);
            }
        }
        BOOST_UBLAS_INLINE
        friend void swap (unit_vector &v1, unit_vector &v2) {
            v1.swap (v2);
        }

        // Iterator types
    private:
        // Use bool to indicate begin (one_ as value)
        typedef bool const_subiterator_type;
    public:
        class const_iterator;

        // Element lookup
        BOOST_UBLAS_INLINE
        const_iterator find (size_type i) const {
            return const_iterator (*this, i == index_);
        }

        class const_iterator:
            public container_const_reference<unit_vector>,
            public bidirectional_iterator_base<sparse_bidirectional_iterator_tag,
                                               const_iterator, value_type> {
        public:
            typedef typename unit_vector::difference_type difference_type;
            typedef typename unit_vector::value_type value_type;
            typedef typename unit_vector::const_reference reference;
            typedef typename unit_vector::const_pointer pointer;

            // Construction and destruction
            BOOST_UBLAS_INLINE
            const_iterator ():
                container_const_reference<unit_vector> (), it_ () {}
            BOOST_UBLAS_INLINE
            const_iterator (const unit_vector &v, const const_subiterator_type &it):
                container_const_reference<unit_vector> (v), it_ (it) {}

            // Arithmetic
            BOOST_UBLAS_INLINE
            const_iterator &operator ++ () {
                BOOST_UBLAS_CHECK (it_, bad_index ());
                it_ = !it_;
                return *this;
            }
            BOOST_UBLAS_INLINE
            const_iterator &operator -- () {
                BOOST_UBLAS_CHECK (!it_, bad_index ());
                it_ = !it_;
                return *this;
            }

            // Dereference
            BOOST_UBLAS_INLINE
            const_reference operator * () const {
                BOOST_UBLAS_CHECK (it_, bad_index ());
                return one_;
            }

            // Index
            BOOST_UBLAS_INLINE
            size_type index () const {
                BOOST_UBLAS_CHECK (it_, bad_index ());
                return (*this) ().index_;
            }

            // Assignment
            BOOST_UBLAS_INLINE
            const_iterator &operator = (const const_iterator &it) {
                container_const_reference<unit_vector>::assign (&it ());
                it_ = it.it_;
                return *this;
            }

            // Comparison
            BOOST_UBLAS_INLINE
            bool operator == (const const_iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ == it.it_;
            }

        private:
            const_subiterator_type it_;
        };

        typedef const_iterator iterator;

        BOOST_UBLAS_INLINE
        const_iterator begin () const {
            return const_iterator (*this, true);
        }
        BOOST_UBLAS_INLINE
        const_iterator end () const {
            return const_iterator (*this, false);
        }

        // Reverse iterator
        typedef reverse_iterator_base<const_iterator> const_reverse_iterator;

        BOOST_UBLAS_INLINE
        const_reverse_iterator rbegin () const {
            return const_reverse_iterator (end ());
        }
        BOOST_UBLAS_INLINE
        const_reverse_iterator rend () const {
            return const_reverse_iterator (begin ());
        }

    private:
        size_type size_;
        size_type index_;
        typedef const value_type const_value_type;
        static const_value_type zero_;
        static const_value_type one_;
    };

    template<class T>
    typename unit_vector<T>::const_value_type unit_vector<T>::zero_ (0);
    template<class T>
    typename unit_vector<T>::const_value_type unit_vector<T>::one_ (1);


    // Scalar vector class
    template<class T>
    class scalar_vector:
        public vector_container<scalar_vector<T> > {

        typedef const T *const_pointer;
        typedef scalar_vector<T> self_type;
    public:
#ifdef BOOST_UBLAS_ENABLE_PROXY_SHORTCUTS
        using vector_container<self_type>::operator ();
#endif
        typedef std::size_t size_type;
        typedef std::ptrdiff_t difference_type;
        typedef T value_type;
        typedef const T &const_reference;
        typedef T &reference;
        typedef const vector_reference<const self_type> const_closure_type;
        typedef dense_tag storage_category;

        // Construction and destruction
        BOOST_UBLAS_INLINE
        scalar_vector ():
            vector_container<self_type> (),
            size_ (0), value_ () {}
        BOOST_UBLAS_INLINE
        explicit scalar_vector (size_type size, const value_type &value = value_type(1)):
            vector_container<self_type> (),
            size_ (size), value_ (value) {}
        BOOST_UBLAS_INLINE
        scalar_vector (const scalar_vector &v):
            vector_container<self_type> (),
            size_ (v.size_), value_ (v.value_) {}

        // Accessors
        BOOST_UBLAS_INLINE
        size_type size () const {
            return size_;
        }

        // Resizing
        BOOST_UBLAS_INLINE
        void resize (size_type size, bool /*preserve*/ = true) {
            size_ = size;
        }

        // Element support
        BOOST_UBLAS_INLINE
        const_pointer find_element (size_type /*i*/) const {
            return & value_;
        }

        // Element access
        BOOST_UBLAS_INLINE
        const_reference operator () (size_type /*i*/) const {
            return value_;
        }

        BOOST_UBLAS_INLINE
        const_reference operator [] (size_type /*i*/) const {
            return value_;
        }

        // Assignment
        BOOST_UBLAS_INLINE
        scalar_vector &operator = (const scalar_vector &v) {
            size_ = v.size_;
            value_ = v.value_;
            return *this;
        }
        BOOST_UBLAS_INLINE
        scalar_vector &assign_temporary (scalar_vector &v) {
            swap (v);
            return *this;
        }

        // Swapping
        BOOST_UBLAS_INLINE
        void swap (scalar_vector &v) {
            if (this != &v) {
                std::swap (size_, v.size_);
                std::swap (value_, v.value_);
            }
        }
        BOOST_UBLAS_INLINE
        friend void swap (scalar_vector &v1, scalar_vector &v2) {
            v1.swap (v2);
        }

        // Iterator types
    private:
        // Use an index
        typedef size_type const_subiterator_type;

    public:
#ifdef BOOST_UBLAS_USE_INDEXED_ITERATOR
        typedef indexed_const_iterator<self_type, dense_random_access_iterator_tag> iterator;
        typedef indexed_const_iterator<self_type, dense_random_access_iterator_tag> const_iterator;
#else
        class const_iterator;
#endif

        // Element lookup
        BOOST_UBLAS_INLINE
        const_iterator find (size_type i) const {
            return const_iterator (*this, i);
        }

#ifndef BOOST_UBLAS_USE_INDEXED_ITERATOR
        class const_iterator:
            public container_const_reference<scalar_vector>,
            public random_access_iterator_base<dense_random_access_iterator_tag,
                                               const_iterator, value_type> {
        public:
            typedef typename scalar_vector::difference_type difference_type;
            typedef typename scalar_vector::value_type value_type;
            typedef typename scalar_vector::const_reference reference;
            typedef typename scalar_vector::const_pointer pointer;

            // Construction and destruction
            BOOST_UBLAS_INLINE
            const_iterator ():
                container_const_reference<scalar_vector> (), it_ () {}
            BOOST_UBLAS_INLINE
            const_iterator (const scalar_vector &v, const const_subiterator_type &it):
                container_const_reference<scalar_vector> (v), it_ (it) {}

            // Arithmetic
            BOOST_UBLAS_INLINE
            const_iterator &operator ++ () {
                ++ it_;
                return *this;
            }
            BOOST_UBLAS_INLINE
            const_iterator &operator -- () {
                -- it_;
                return *this;
            }
            BOOST_UBLAS_INLINE
            const_iterator &operator += (difference_type n) {
                it_ += n;
                return *this;
            }
            BOOST_UBLAS_INLINE
            const_iterator &operator -= (difference_type n) {
                it_ -= n;
                return *this;
            }
            BOOST_UBLAS_INLINE
            difference_type operator - (const const_iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ - it.it_;
            }

            // Dereference
            BOOST_UBLAS_INLINE
            const_reference operator * () const {
                BOOST_UBLAS_CHECK (it_ < (*this) ().size (), bad_index ());
                return (*this) () (index ());
            }
            BOOST_UBLAS_INLINE
            const_reference operator [] (difference_type n) const {
                return *(*this + n);
            }

            // Index
            BOOST_UBLAS_INLINE
            size_type index () const {
                BOOST_UBLAS_CHECK (it_ < (*this) ().size (), bad_index ());
                return it_;
            }

            // Assignment
            BOOST_UBLAS_INLINE
            const_iterator &operator = (const const_iterator &it) {
                container_const_reference<scalar_vector>::assign (&it ());
                it_ = it.it_;
                return *this;
            }

            // Comparison
            BOOST_UBLAS_INLINE
            bool operator == (const const_iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ == it.it_;
            }
            BOOST_UBLAS_INLINE
            bool operator < (const const_iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ < it.it_;
            }

        private:
            const_subiterator_type it_;
        };

        typedef const_iterator iterator;
#endif

        BOOST_UBLAS_INLINE
        const_iterator begin () const {
            return find (0);
        }
        BOOST_UBLAS_INLINE
        const_iterator end () const {
            return find (size_);
        }

        // Reverse iterator
        typedef reverse_iterator_base<const_iterator> const_reverse_iterator;

        BOOST_UBLAS_INLINE
        const_reverse_iterator rbegin () const {
            return const_reverse_iterator (end ());
        }
        BOOST_UBLAS_INLINE
        const_reverse_iterator rend () const {
            return const_reverse_iterator (begin ());
        }

    private:
        size_type size_;
        value_type value_;
    };


    // Array based vector class
    template<class T, std::size_t N>
    class c_vector:
        public vector_container<c_vector<T, N> > {

        typedef c_vector<T, N> self_type;
    public:
#ifdef BOOST_UBLAS_ENABLE_PROXY_SHORTCUTS
        using vector_container<self_type>::operator ();
#endif
        typedef std::size_t size_type;
        typedef std::ptrdiff_t difference_type;
        typedef T value_type;
        typedef const T &const_reference;
        typedef T &reference;
        typedef value_type array_type[N];
        typedef T *pointer;
        typedef const T *const_pointer;
        typedef const vector_reference<const self_type> const_closure_type;
        typedef vector_reference<self_type> closure_type;
        typedef self_type vector_temporary_type;
        typedef dense_tag storage_category;

        // Construction and destruction
        BOOST_UBLAS_INLINE
        c_vector ():
            size_ (N) /* , data_ () */ {}
        explicit BOOST_UBLAS_INLINE
        c_vector (size_type size):
            size_ (size) /* , data_ () */ {
            if (size_ > N)
                bad_size ().raise ();
        }
        BOOST_UBLAS_INLINE
        c_vector (const c_vector &v):
            size_ (v.size_) /* , data_ () */ {
            if (size_ > N)
                bad_size ().raise ();
            *this = v;
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        c_vector (const vector_expression<AE> &ae):
            size_ (ae ().size ()) /* , data_ () */ {
            if (size_ > N)
                bad_size ().raise ();
            vector_assign<scalar_assign> (*this, ae);
        }

        // Accessors
        BOOST_UBLAS_INLINE
        size_type size () const {
            return size_;
        }
        BOOST_UBLAS_INLINE
        const_pointer data () const {
            return data_;
        }
        BOOST_UBLAS_INLINE
        pointer data () {
            return data_;
        }

        // Resizing
        BOOST_UBLAS_INLINE
        void resize (size_type size, bool preserve = true) {
            if (size > N)
                bad_size ().raise ();
            size_ = size;
        }

        // Element support
        BOOST_UBLAS_INLINE
        pointer find_element (size_type i) {
            return const_cast<pointer> (const_cast<const self_type&>(*this).find_element (i));
        }
        BOOST_UBLAS_INLINE
        const_pointer find_element (size_type i) const {
            return & data_ [i];
        }

        // Element access
        BOOST_UBLAS_INLINE
        const_reference operator () (size_type i) const {
            BOOST_UBLAS_CHECK (i < size_,  bad_index ());
            return data_ [i];
        }
        BOOST_UBLAS_INLINE
        reference operator () (size_type i) {
            BOOST_UBLAS_CHECK (i < size_, bad_index ());
            return data_ [i];
        }

        BOOST_UBLAS_INLINE
        const_reference operator [] (size_type i) const {
            return (*this) (i);
        }
        BOOST_UBLAS_INLINE
        reference operator [] (size_type i) {
            return (*this) (i);
        }

        // Element assignment
        BOOST_UBLAS_INLINE
        reference insert_element (size_type i, const_reference t) {
            BOOST_UBLAS_CHECK (i < size_, bad_index ());
            return (data_ [i] = t);
        }
        BOOST_UBLAS_INLINE
        void erase_element (size_type i) {
            BOOST_UBLAS_CHECK (i < size_, bad_index ());
            data_ [i] = value_type/*zero*/();
        }
        
        // Zeroing
        BOOST_UBLAS_INLINE
        void clear () {
            std::fill (data_, data_ + size_, value_type/*zero*/());
        }

        // Assignment
        BOOST_UBLAS_INLINE
        c_vector &operator = (const c_vector &v) {
            size_ = v.size_;
            std::copy (v.data_, v.data_ + v.size_, data_);
            return *this;
        }
        template<class C>          // Container assignment without temporary
        BOOST_UBLAS_INLINE
        c_vector &operator = (const vector_container<C> &v) {
            resize (v ().size (), false);
            assign (v);
            return *this;
        }
        BOOST_UBLAS_INLINE
        c_vector &assign_temporary (c_vector &v) {
            swap (v);
            return *this;
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        c_vector &operator = (const vector_expression<AE> &ae) {
            self_type temporary (ae);
            return assign_temporary (temporary);
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        c_vector &assign (const vector_expression<AE> &ae) {
            vector_assign<scalar_assign> (*this, ae);
            return *this;
        }

        // Computed assignment
        template<class AE>
        BOOST_UBLAS_INLINE
        c_vector &operator += (const vector_expression<AE> &ae) {
            self_type temporary (*this + ae);
            return assign_temporary (temporary);
        }
        template<class C>          // Container assignment without temporary
        BOOST_UBLAS_INLINE
        c_vector &operator += (const vector_container<C> &v) {
            plus_assign (v);
            return *this;
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        c_vector &plus_assign (const vector_expression<AE> &ae) {
            vector_assign<scalar_plus_assign> ( *this, ae);
            return *this;
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        c_vector &operator -= (const vector_expression<AE> &ae) {
            self_type temporary (*this - ae);
            return assign_temporary (temporary);
        }
        template<class C>          // Container assignment without temporary
        BOOST_UBLAS_INLINE
        c_vector &operator -= (const vector_container<C> &v) {
            minus_assign (v);
            return *this;
        }
        template<class AE>
        BOOST_UBLAS_INLINE
        c_vector &minus_assign (const vector_expression<AE> &ae) {
            vector_assign<scalar_minus_assign> (*this, ae);
            return *this;
        }
        template<class AT>
        BOOST_UBLAS_INLINE
        c_vector &operator *= (const AT &at) {
            vector_assign_scalar<scalar_multiplies_assign> (*this, at);
            return *this;
        }
        template<class AT>
        BOOST_UBLAS_INLINE
        c_vector &operator /= (const AT &at) {
            vector_assign_scalar<scalar_divides_assign> (*this, at);
            return *this;
        }

        // Swapping
        BOOST_UBLAS_INLINE
        void swap (c_vector &v) {
            if (this != &v) {
                BOOST_UBLAS_CHECK (size_ == v.size_, bad_size ());
                std::swap (size_, v.size_);
                std::swap_ranges (data_, data_ + size_, v.data_);
            }
        }
        BOOST_UBLAS_INLINE
        friend void swap (c_vector &v1, c_vector &v2) {
            v1.swap (v2);
        }

        // Iterator types
    private:
        // Use pointers for iterator
        typedef const_pointer const_subiterator_type;
        typedef pointer subiterator_type;

    public:
#ifdef BOOST_UBLAS_USE_INDEXED_ITERATOR
        typedef indexed_iterator<self_type, dense_random_access_iterator_tag> iterator;
        typedef indexed_const_iterator<self_type, dense_random_access_iterator_tag> const_iterator;
#else
        class const_iterator;
        class iterator;
#endif

        // Element lookup
        BOOST_UBLAS_INLINE
        const_iterator find (size_type i) const {
#ifndef BOOST_UBLAS_USE_INDEXED_ITERATOR
            return const_iterator (*this, &data_ [i]);
#else
            return const_iterator (*this, i);
#endif
        }
        BOOST_UBLAS_INLINE
        iterator find (size_type i) {
#ifndef BOOST_UBLAS_USE_INDEXED_ITERATOR
            return iterator (*this, &data_ [i]);
#else
            return iterator (*this, i);
#endif
        }

#ifndef BOOST_UBLAS_USE_INDEXED_ITERATOR
        class const_iterator:
            public container_const_reference<c_vector>,
            public random_access_iterator_base<dense_random_access_iterator_tag,
                                               const_iterator, value_type> {
        public:
            typedef typename c_vector::difference_type difference_type;
            typedef typename c_vector::value_type value_type;
            typedef typename c_vector::const_reference reference;
            typedef typename c_vector::const_pointer pointer;

            // Construction and destruction
            BOOST_UBLAS_INLINE
            const_iterator ():
                container_const_reference<self_type> (), it_ () {}
            BOOST_UBLAS_INLINE
            const_iterator (const self_type &v, const const_subiterator_type &it):
                container_const_reference<self_type> (v), it_ (it) {}
            BOOST_UBLAS_INLINE
            const_iterator (const typename self_type::iterator &it):  // ISSUE self_type:: stops VC8 using std::iterator here
                container_const_reference<self_type> (it ()), it_ (it.it_) {}

            // Arithmetic
            BOOST_UBLAS_INLINE
            const_iterator &operator ++ () {
                ++ it_;
                return *this;
            }
            BOOST_UBLAS_INLINE
            const_iterator &operator -- () {
                -- it_;
                return *this;
            }
            BOOST_UBLAS_INLINE
            const_iterator &operator += (difference_type n) {
                it_ += n;
                return *this;
            }
            BOOST_UBLAS_INLINE
            const_iterator &operator -= (difference_type n) {
                it_ -= n;
                return *this;
            }
            BOOST_UBLAS_INLINE
            difference_type operator - (const const_iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ - it.it_;
            }

            // Dereference
            BOOST_UBLAS_INLINE
            const_reference operator * () const {
                BOOST_UBLAS_CHECK (it_ >= (*this) ().begin ().it_ && it_ < (*this) ().end ().it_, bad_index ());
                return *it_;
            }
            BOOST_UBLAS_INLINE
            const_reference operator [] (difference_type n) const {
                return *(it_ + n);
            }

            // Index
            BOOST_UBLAS_INLINE
            size_type index () const {
                BOOST_UBLAS_CHECK (it_ >= (*this) ().begin ().it_ && it_ < (*this) ().end ().it_, bad_index ());
                const self_type &v = (*this) ();
                return it_ - v.begin ().it_;
            }

            // Assignment
            BOOST_UBLAS_INLINE
            const_iterator &operator = (const const_iterator &it) {
                container_const_reference<self_type>::assign (&it ());
                it_ = it.it_;
                return *this;
            }

            // Comparison
            BOOST_UBLAS_INLINE
            bool operator == (const const_iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ == it.it_;
            }
            BOOST_UBLAS_INLINE
            bool operator < (const const_iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ < it.it_;
            }

        private:
            const_subiterator_type it_;

            friend class iterator;
        };
#endif

        BOOST_UBLAS_INLINE
        const_iterator begin () const {
            return find (0);
        }
        BOOST_UBLAS_INLINE
        const_iterator end () const {
            return find (size_);
        }

#ifndef BOOST_UBLAS_USE_INDEXED_ITERATOR
        class iterator:
            public container_reference<c_vector>,
            public random_access_iterator_base<dense_random_access_iterator_tag,
                                               iterator, value_type> {
        public:
            typedef typename c_vector::difference_type difference_type;
            typedef typename c_vector::value_type value_type;
            typedef typename c_vector::reference reference;
            typedef typename c_vector::pointer pointer;

            // Construction and destruction
            BOOST_UBLAS_INLINE
            iterator ():
                container_reference<self_type> (), it_ () {}
            BOOST_UBLAS_INLINE
            iterator (self_type &v, const subiterator_type &it):
                container_reference<self_type> (v), it_ (it) {}

            // Arithmetic
            BOOST_UBLAS_INLINE
            iterator &operator ++ () {
                ++ it_;
                return *this;
            }
            BOOST_UBLAS_INLINE
            iterator &operator -- () {
                -- it_;
                return *this;
            }
            BOOST_UBLAS_INLINE
            iterator &operator += (difference_type n) {
                it_ += n;
                return *this;
            }
            BOOST_UBLAS_INLINE
            iterator &operator -= (difference_type n) {
                it_ -= n;
                return *this;
            }
            BOOST_UBLAS_INLINE
            difference_type operator - (const iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ - it.it_;
            }

            // Dereference
            BOOST_UBLAS_INLINE
            reference operator * () const {
                BOOST_UBLAS_CHECK (it_ >= (*this) ().begin ().it_ && it_ < (*this) ().end ().it_, bad_index ());
                return *it_;
            }
            BOOST_UBLAS_INLINE
            reference operator [] (difference_type n) const {
                return *(it_ + n);
            }

            // Index
            BOOST_UBLAS_INLINE
            size_type index () const {
                BOOST_UBLAS_CHECK (it_ >= (*this) ().begin ().it_ && it_ < (*this) ().end ().it_, bad_index ());
                // EDG won't allow const self_type it doesn't allow friend access to it_
                self_type &v = (*this) ();
                return it_ - v.begin ().it_;
            }

            // Assignment
            BOOST_UBLAS_INLINE
            iterator &operator = (const iterator &it) {
                container_reference<self_type>::assign (&it ());
                it_ = it.it_;
                return *this;
            }

            // Comparison
            BOOST_UBLAS_INLINE
            bool operator == (const iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ == it.it_;
            }
            BOOST_UBLAS_INLINE
            bool operator < (const iterator &it) const {
                BOOST_UBLAS_CHECK (&(*this) () == &it (), external_logic ());
                return it_ < it.it_;
            }

        private:
            subiterator_type it_;

            friend class const_iterator;
        };
#endif

        BOOST_UBLAS_INLINE
        iterator begin () {
            return find (0);
        }
        BOOST_UBLAS_INLINE
        iterator end () {
            return find (size_);
        }

        // Reverse iterator
        typedef reverse_iterator_base<const_iterator> const_reverse_iterator;
        typedef reverse_iterator_base<iterator> reverse_iterator;

        BOOST_UBLAS_INLINE
        const_reverse_iterator rbegin () const {
            return const_reverse_iterator (end ());
        }
        BOOST_UBLAS_INLINE
        const_reverse_iterator rend () const {
            return const_reverse_iterator (begin ());
        }
        BOOST_UBLAS_INLINE
        reverse_iterator rbegin () {
            return reverse_iterator (end ());
        }
        BOOST_UBLAS_INLINE
        reverse_iterator rend () {
            return reverse_iterator (begin ());
        }

    private:
        size_type size_;
        array_type data_;
    };

}}}

#endif
