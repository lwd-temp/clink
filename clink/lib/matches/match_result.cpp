/* Copyright (c) 2015 Martin Ridgers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pch.h"
#include "match_result.h"
#include "core/str.h"

#include <algorithm>

//------------------------------------------------------------------------------
match_result::match_result()
{
}

//------------------------------------------------------------------------------
match_result::match_result(match_result&& rhs)
{
    swap(rhs);
}

//------------------------------------------------------------------------------
match_result::~match_result()
{
    for (int i = 0, e = int(m_matches.size()); i < e; ++i)
        delete m_matches[i];
}

//------------------------------------------------------------------------------
void match_result::operator = (match_result&& rhs)
{
    swap(rhs);
}

//------------------------------------------------------------------------------
void match_result::swap(match_result& rhs)
{
    std::swap(m_matches, rhs.m_matches);
}

//------------------------------------------------------------------------------
void match_result::reserve(unsigned int count)
{
    m_matches.reserve(count);
}

//------------------------------------------------------------------------------
void match_result::add_match(const char* match)
{
    int len = int(strlen(match)) + 1;
    char* out = new char[len];
    str_base(out, len).copy(match);
    m_matches.push_back(out);
}
