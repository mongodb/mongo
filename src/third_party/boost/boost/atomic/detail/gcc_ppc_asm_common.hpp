/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2021 Andrey Semashev
 */
/*!
 * \file   atomic/detail/gcc_ppc_asm_common.hpp
 *
 * This header contains basic utilities for gcc asm-based PowerPC backend.
 */

#ifndef BOOST_ATOMIC_DETAIL_GCC_PPC_ASM_COMMON_HPP_INCLUDED_
#define BOOST_ATOMIC_DETAIL_GCC_PPC_ASM_COMMON_HPP_INCLUDED_

#include <boost/atomic/detail/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if !defined(_AIX)
#define BOOST_ATOMIC_DETAIL_PPC_ASM_LABEL(label) label ":\n\t"
#define BOOST_ATOMIC_DETAIL_PPC_ASM_JUMP(insn, label, offset) insn " " label "\n\t"
#else
// Standard assembler tool (as) on AIX does not support numeric jump labels, so we have to use offsets instead.
// https://github.com/boostorg/atomic/pull/50
#define BOOST_ATOMIC_DETAIL_PPC_ASM_LABEL(label)
#define BOOST_ATOMIC_DETAIL_PPC_ASM_JUMP(insn, label, offset) insn " $" offset "\n\t"
#endif

#endif // BOOST_ATOMIC_DETAIL_GCC_PPC_ASM_COMMON_HPP_INCLUDED_
