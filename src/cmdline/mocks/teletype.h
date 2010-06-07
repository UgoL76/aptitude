/** \file teletype.h */          // -*-c++-*-

// Copyright (C) 2010 Daniel Burrows
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; see the file COPYING.  If not, write to
// the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.

#ifndef APTITUDE_CMDLINE_MOCKS_TELETYPE_H
#define APTITUDE_CMDLINE_MOCKS_TELETYPE_H

// System includes:
#include <boost/algorithm/string.hpp>
#include <boost/shared_ptr.hpp>

#include <gmock/gmock.h>

namespace aptitude
{
  namespace cmdline
  {
    namespace mocks
    {
      class terminal;
      class terminal_locale;

      inline std::string make_str(const char *s)
      {
        return std::string(s);
      }

      inline std::wstring make_str(const wchar_t *s)
      {
        return std::wstring(s);
      }

      template<typename T>
      inline T make_str(const T &t)
      {
        return t;
      }

      // Defined here because it's meant for use with this class.
      MATCHER_P(StrTrimmedEq, str, "is equal after trimming to %(str)s")
      {
        return boost::equals(boost::trim_copy(make_str(str)),
                             boost::trim_copy(make_str(arg)));
      }

      /** \brief An adapter to assist testing the output sent to a
       *  terminal in terms of its effect on a hypothetical line-based
       *  output device that allows overwrites.
       */
      class teletype
      {
        // Q: Should we also track the cursor position?
      public:
        virtual ~teletype();

        /** \brief Invoked when the active line of text is modified. */
        MOCK_METHOD1(set_last_line, void(const std::wstring &));

        /** \brief Invoked when the active line of text is advanced.
         *
         *  After this is invoked, the previous active line is the
         *  last inactive line, and the active line is empty.
         */
        MOCK_METHOD0(newline, void());
      };

      /** \brief Create a teletype mock specialized to process the
       *  output from the given terminal.
       *
       *  Also adds expectations on the terminal to ignore all calls
       *  to output() and flush(), so they don't generate errors.
       */
      boost::shared_ptr<teletype>
      create_teletype(const boost::shared_ptr<terminal> &term,
                      const boost::shared_ptr<terminal_locale> &term_locale);
    }
  }
}

#endif // APTITUDE_CMDLINE_MOCKS_TELETYPE_H
