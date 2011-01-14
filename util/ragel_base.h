#ifndef __RagelBase_HPP__
#define __RagelBase_HPP__

/// \file RagelBase.hpp

/*
 *    Copyright 2007-2011 Intrusion Inc
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Lesser General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */


//#include <stdint.h>
//#include <stdio.h>
//#include <ctype.h>

// The user of this file should define RAGEL_MAX_STACK and RAGEL_MAX_TOKEN
// before including this file.
// Define default values in case they dont.

#ifndef RAGEL_MAX_STACK
#define RAGEL_MAX_STACK 16
#endif

#ifndef RAGEL_MAX_TOKEN
#define RAGEL_MAX_TOKEN 16
#endif


template < class CharType >
class RagelBase
{
  public:

    RagelBase() {}
    virtual ~RagelBase() {}

    /// Perform initialization
    ///
    virtual void init(void)
    {
        ts = NULL;
        te = NULL;
        eof = NULL;
        m_token_len = 0;
        m_parser_error = false;
    }


    /// Set internal pointers to the buffer to be parsed, prior to calling execute().
    /// \param p_buffer_start A pointer to the first character of the buffer
    /// \param p_buffer_len The number of characters in the buffer.
    /// \param p_eof If true then set the internal EOF flag, defaults to false.
    ///
    void setBuffer(const CharType* p_buffer_start, int p_buffer_len, bool p_eof = false)
    {
        m_buffer_start = p_buffer_start;
        m_buffer_end = p_buffer_start + p_buffer_len;
        p = (CharType*)m_buffer_start;
        pe = m_buffer_end;
        m_last_p = p;

        if (p_eof)
        {
            // Set the End Of File pointer
            eof = pe;
        }
    }


    /// Execute the state machine on the buffer set by setBuffer().
    /// \returns True on success, false on a parser error or
    ///          token buffer overrrun.
    ///
    bool execute(void)
    {
        m_exit_parser = false;

        if (eof && (p == pe))
        {
            // This is a special case where we want to coerce the machine
            // into the eof state.
            m_exit_parser = true;
            goto execute;
        }

        while ((m_last_p < m_buffer_end) && (p != m_buffer_end) && !m_exit_parser)
        {
            // Refer to section 6.3 in the Ragel Guide
            if ((m_token_len > 0) && (m_token_len < m_max_token))
            {
                // INTZ_DEBUG_ASSERT(m_last_p);

                // The last buffer stopped in a scanner
                // Copy the next character from the user's buffer
                // and append it to the token buffer in an attempt to
                // complete the current scanner token.
                p = &m_token_buf[m_token_len];
                m_token_buf[m_token_len++] = *m_last_p++;
                pe = &m_token_buf[m_token_len];
            }

        execute:
            // Execute the Ragel engine
            ragel_exec();

            if (parserError() || m_parser_error)
            {
                // Parse error
                return false;
            }

            if (ts && (te > ts))
            {
                // The last buffer ended inside a scanner
                m_token_len = (int)(pe - ts);

                if (m_token_len < 0)
                {
                    // This might indicate an error condition.
                    //GDB_TRAP;
                    m_token_len = 0;
                    return false;
                }

                if (m_token_len >= m_max_token)
                {
                    // Arriving here means that our buffer is too small to hold
                    // the partial token being scanned. You should
                    // allocate more space by defining RAGEL_MAX_TOKEN.
                    //GDB_TRAP;
                    return false;
                }

                // Copy the partially scanned token to the beginning of
                // the token buffer.  This may be a copy from the user's buffer,
                // or a copy from within the token buffer itself.
                if (ts != m_token_buf)
                {
                    memmove(m_token_buf, ts, m_token_len);
                    te = m_token_buf + (te - ts);
                    ts = m_token_buf;
                }
            }
            else
            if (m_token_len)
            {
                // No scanner in progress
                m_token_len = 0;

                // Reset p and pe back to the user's buffer
                p = (CharType*)m_last_p;
                pe = m_buffer_end;
            }
        }

        // We've consumed all of the user's buffer.
        return true;
    }



    /// This must be over-ridden by the inheriting class.
    virtual void ragel_exec(void)
    {
        //GDB_TRAP;
    }


    /// This must be over-ridden by the inheriting class.
    /// \returns True if the call to ragel_exec() failed.
    virtual bool parserError(void)
    {
        //GDB_TRAP;
        return true;
    }


    /// Declare an error.
    void setParserError(void)
    {
        m_parser_error = true;
    }


    /// Immediatly set the EOF state.  The execute() function must be called after
    /// calling setEOF() to execute EOF actions.
    ///
    void setEOF(void)
    {
        eof = pe;
        p = (CharType*)pe;
    }


    /// \returns True if the end of the current buffer has been reached
    ///
    bool endOfBuffer(void) { return p == pe; }


    /// \returns True if the end of file has been reached
    ///
    bool isEOF(void) { return p == eof; }


    /// \returns A pointer to the current position of this parser.  The caller must check
    ///          if the value returned is past the buffer provided, which would indicate
    ///          all data was consumed.
    ///
    CharType* getCurrentPosition(void) { return p; }


    /// \returns The number of bytes in the buffer that have not been processed
    ///          by the parser.
    ///
    unsigned int getBytesRemaining(void)
    {
        if (isEOF())
            return 0;

        return (unsigned int)(pe - p);
    }


    /// Advance the machine's current buffer byte pointer.  Will not allow the pointer
    /// to advance past the end of the buffer.
    /// \param p_byte_advance Number of bytes to advance the pointer.
    ///
    void advancePointer(unsigned int p_byte_advance)
    {
        p += p_byte_advance;

        if (p > pe)
            p = (CharType*)pe;
    }


    /// Action to be performed before the Ragel machine pushes onto the scanner stack
    void prepush(void)
    {
        // prepush
        // Make sure the Ragel stack will not overflow
        // This will ruin the outcome of the machine,
        // but will prevent stepping outside the stack.
        // Arriving here means that our stack is too small.
        // You should allocate more space by defining RAGEL_MAX_STACK.
        if (top >= m_max_stack)
        {
            //GDB_TRAP;
            --top;
        }
    }

    /// Actions to be performed after the Ragel machine pops from the scanner stack
    void postpop(void)
    {
        // Check for Ragel stack underflow
        if (top < 0)
        {
            //GDB_TRAP;
            ++top;
        }
    }


  protected:

    // Maximum size of the stack
    const static int m_max_stack = RAGEL_MAX_STACK;

    // Maximum size of the token buffer
    const static int m_max_token = RAGEL_MAX_TOKEN;

    // Current buffer information
    const CharType* m_buffer_start;
    const CharType* m_buffer_end;
    const CharType* m_last_p;

    // Ragel buffer pointers
    CharType*       p;                  // Next input character
    const CharType* pe;                 // End of buffer
    const CharType* eof;                // Used when EOF actions are present
    CharType*       ts;                 // Used by scanners
    CharType*       te;                 // Used by scanners
    int             cs;                 // Integer state
    int             stack[m_max_stack]; // State Stack
    int             top;                // Top of stack index
    int             act;                // Used by scanners
    int             m_token_len;        // Used internally to indicate number of chars in m_token_buf;
    CharType        m_token_buf[m_max_token]; // A small buffer to hold tokens being scanned between buffers
    bool            m_exit_parser;      // Can be set true to prematurely exit the execute() loop
    bool            m_parser_error;     // Indicates a parser error, will exit the execute() loop
};


#endif // __RagelBase_HPP__


