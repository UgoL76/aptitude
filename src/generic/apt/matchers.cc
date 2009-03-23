// matchers.cc
//
//  Copyright 2000-2005, 2007-2008 Daniel Burrows
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; see the file COPYING.  If not, write to
//  the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
//  Boston, MA 02111-1307, USA.
//
//  Grammer for the condition language.  (TODO: this is what the
//  grammar *will* be, not what it is)
//
//  CONDITION := CONDITION-LIST
//  CONDITION-LIST := CONDITION-AND-GROUP '|' CONDITION-LIST
//                 |  CONDITION-AND-GROUP
//  CONDITION-AND-GROUP := CONDITION-ATOM CONDITION-AND-GROUP
//                      := CONDITION-ATOM
//  CONDITION-ATOM := '(' CONDITION-LIST ')'
//                 |  '!' CONDITION-ATOM
//                 |  '?for' variable-name ':' CONDITION-LIST
//                 |  '?=' variable-name
//                 |  '?' (variable-name ':')?  condition-name '(' arguments... ')'
//                 |  '~'field-id <string>
//                 |  <string>
//
// The (arguments...) to a ?function-style matcher are parsed
// according to their expected type.  This is unfortunate but
// necessary: since arbitrary strings not containing metacharacters
// are legal condition values, distinguishing conditions from other
// argument types would require the user to type extra punctuation in,
// e.g., ?broken(Depends, ?name(apt.*)).

#include "matchers.h"

#include "apt.h"
#include "aptcache.h"
#include "tags.h"
#include "tasks.h"

#include <aptitude.h>

#include <generic/util/immset.h>
#include <generic/util/util.h>

#include <cwidget/generic/util/ssprintf.h>
#include <cwidget/generic/util/transcode.h>

#include <set>

#include <stdarg.h>

#include <apt-pkg/error.h>
#include <apt-pkg/pkgcache.h>
#include <apt-pkg/pkgrecords.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/version.h>

#include <cwidget/generic/util/eassert.h>
#include <ctype.h>
#include <stdio.h>
#include <regex.h>
#include <sys/types.h>

using namespace std;
namespace cw = cwidget;

namespace aptitude
{
namespace matching
{

pkg_match_result::~pkg_match_result()
{
}

pkg_matcher::~pkg_matcher()
{
}

namespace
{
// The condition this represents should never ever happen; this is
// just to provide a well-defined message in case it does so I can
// track it down.
class BadValueTypeException : public cw::util::Exception
{
  std::string msg;
public:
  BadValueTypeException(int type,
			const std::string &file,
			int line_number)
    : msg(ssprintf("%s:%d: Internal error: unknown value type %d.",
		   file.c_str(), line_number, type))
  {
  }

  std::string errmsg() const { return msg; }
};

#define THROW_BAD_VALUE(type) throw BadValueTypeException((type), __FILE__, __LINE__)

/** An object describing a matching rule.  Note that we match on a
 *  particular version, not just on the package (this is because some
 *  attributes are pretty meaningless for only a package)
 */
class pkg_matcher_real : public pkg_matcher
{
public:
  class stack_value;
  typedef std::vector<stack_value> match_stack;

  // The type of values stored on the match stack.
  //
  // This is basically a proto-value-type for the match language; if
  // the match language grows into an implementation of the full
  // lambda calculus, it will need to include more types (at the
  // minimum, it'll need a type for matchers).
  class stack_value
  {
    enum value_type
      {
	package_value,
	version_value
      };

    value_type type;
    pkgCache::PkgIterator pkg;
    pkgCache::VerIterator ver;

    stack_value(value_type _type, const pkgCache::PkgIterator &_pkg, const pkgCache::VerIterator &_ver)
      : type(_type), pkg(_pkg), ver(_ver)
    {
    }

  public:
    static stack_value package(const pkgCache::PkgIterator &pkg)
    {
      return stack_value(package_value, pkg, pkgCache::VerIterator(*const_cast<pkgCache::PkgIterator &>(pkg).Cache()));
    }

    static stack_value version(const pkgCache::PkgIterator &pkg,
			       const pkgCache::VerIterator &ver)
    {
      return stack_value(version_value, pkg, ver);
    }

    /** \brief Return \b true if this value "matches" the
     *  given value.
     *
     *  This relation is reflexive and symmetric, but not transitive.
     *
     *  Packages match any of their versions or themselves, versions
     *  match themselves and their package.
     */
    bool is_match_for(const stack_value &other) const
    {
      switch(type)
	{
	case package_value:
	  switch(other.type)
	    {
	    case package_value:
	      return pkg == other.pkg;
	    case version_value:
	      return pkg == other.pkg;
	    default:
	      THROW_BAD_VALUE(other.type);
	    }
	case version_value:
	  switch(other.type)
	    {
	    case package_value:
	      return pkg == other.pkg;
	    case version_value:
	      return pkg == other.pkg && ver == other.ver;
	    default:
	      THROW_BAD_VALUE(other.type);
	    }
	default:
	  THROW_BAD_VALUE(type);
	}
    }


    bool visit_matches(pkg_matcher_real *matcher,
		       aptitudeDepCache &cache,
		       pkgRecords &records,
		       match_stack &stack) const;

    pkg_match_result *visit_get_match(pkg_matcher_real *matcher,
				      aptitudeDepCache &cache,
				      pkgRecords &records,
				      match_stack &stack) const;
  };

  virtual bool matches(const pkgCache::PkgIterator &pkg,
		       const pkgCache::VerIterator &ver,
		       aptitudeDepCache &cache,
		       pkgRecords &records,
		       match_stack &stack)=0;



  /** \return a match result, or \b NULL if there is no match.  It's
   *  the caller's responsibility to delete this.
   */
  virtual pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
				      const pkgCache::VerIterator &ver,
				      aptitudeDepCache &cache,
				      pkgRecords &records,
				      match_stack &stack)=0;

  /** See whether this matches a versionless package.  This applies
   *  the matcher to every version of the package and returns \b true
   *  if any version is matched.
   */
  virtual bool matches(const pkgCache::PkgIterator &pkg,
		       aptitudeDepCache &cache,
		       pkgRecords &records,
		       match_stack &stack);

  /** Get a match result for a versionless package.  This applies the
   *  matcher to each version of the package, returning \b NULL if
   *  none matches or the first match found otherwise.
   */
  virtual pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
				      aptitudeDepCache &cache,
				      pkgRecords &records,
				      match_stack &stack);
};

bool pkg_matcher_real::stack_value::visit_matches(pkg_matcher_real *matcher,
						  aptitudeDepCache &cache,
						  pkgRecords &records,
						  match_stack &stack) const
{
  switch(type)
    {
    case package_value:
      return matcher->matches(pkg, cache, records, stack);
    case version_value:
      return matcher->matches(pkg, ver, cache, records, stack);
    default:
      THROW_BAD_VALUE(type);
    }
}

pkg_match_result *pkg_matcher_real::stack_value::visit_get_match(pkg_matcher_real *matcher,
								 aptitudeDepCache &cache,
								 pkgRecords &records,
								 match_stack &stack) const
{
  switch(type)
    {
    case package_value:
      return matcher->get_match(pkg, cache, records, stack);
    case version_value:
      return matcher->get_match(pkg, ver, cache, records, stack);
    default:
      THROW_BAD_VALUE(type);
    }
}

typedef imm::map<std::string, int> parse_environment;

pkg_matcher_real *parse_atom(string::const_iterator &start,
			     const string::const_iterator &end,
			     const vector<const char *> &terminators,
			     bool search_descriptions,
			     bool wide_context,
			     const parse_environment &name_context);

pkg_matcher_real *parse_condition_list(string::const_iterator &start,
				       const string::const_iterator &end,
				       const vector<const char *> &terminators,
				       bool search_descriptions,
				       bool wide_context,
				       const parse_environment &name_context);

/** Used to cleanly abort without having to contort the code. */
class CompilationException
{
  string reason;
public:
  CompilationException(const char *format,
		       ...)
#ifdef __GNUG__
    __attribute__ ((format (printf, 2, 3)))
#endif
  {
    va_list args;

    va_start(args, format);

    reason = cw::util::vssprintf(format, args);
  }

  const string &get_msg() {return reason;}
};

namespace
{
  /** \brief Enumeration containing the known types of
   *  matchers.
   *
   *  I want to have a table mapping matcher names to matcher types,
   *  and so the matcher type has to be POD.  Well, I could use some
   *  indirection pattern like grouping policies do, but IMNSHO the
   *  payoff in grouping-policy land has not made up for the syntactic
   *  clutter and semantic overhead.  I think that if anything it
   *  would be less valuable here.
   *
   *  Note that matchers for dependencies and for broken dependencies
   *  are parsed separately below.
   */
  enum matcher_type
    {
      matcher_type_action,
      matcher_type_all,
      matcher_type_and,
      matcher_type_any,
      matcher_type_archive,
      matcher_type_automatic,
      matcher_type_bind,
      matcher_type_broken,
      matcher_type_config_files,
      matcher_type_description,
      matcher_type_essential,
      matcher_type_false,
      matcher_type_for,
      matcher_type_garbage,
      matcher_type_installed,
      matcher_type_maintainer,
      matcher_type_name,
      matcher_type_narrow,
      matcher_type_new,
      matcher_type_not,
      matcher_type_obsolete,
      matcher_type_or,
      matcher_type_origin,
      matcher_type_priority,
      matcher_type_provides,
      matcher_type_section,
      matcher_type_source_package,
      matcher_type_source_version,
      matcher_type_tag,
      matcher_type_task,
      matcher_type_true,
      matcher_type_upgradable,
      matcher_type_user_tag,
      matcher_type_version,
      matcher_type_virtual,
      matcher_type_widen
    };

  struct matcher_info
  {
    /** \brief The string used to pick the matcher.
     */
    const char *name;

    /** \brief The matcher type indicated by this struct. */
    matcher_type type;
  };

  const matcher_info matcher_types[] =
  {
    { "action", matcher_type_action },
    { "all-versions", matcher_type_all },
    { "and", matcher_type_and },
    { "any-version", matcher_type_any },
    { "archive", matcher_type_archive },
    { "automatic", matcher_type_automatic },
    { "bind", matcher_type_bind },
    { "broken", matcher_type_broken },
    { "config-files", matcher_type_config_files },
    { "description", matcher_type_description },
    { "essential", matcher_type_essential },
    { "false", matcher_type_false },
    // ForTranslators: As in the sentence "for x = 5, do BLAH".
    { "for", matcher_type_for },
    { "garbage", matcher_type_garbage },
    { "installed", matcher_type_installed },
    { "maintainer", matcher_type_maintainer },
    { "name", matcher_type_name },
    /* ForTranslators: Opposite of widen. Search for "widen" in this file for details. */
    { "narrow", matcher_type_narrow },
    { "new", matcher_type_new },
    { "not", matcher_type_not },
    { "obsolete", matcher_type_obsolete },
    { "or", matcher_type_or },
    /* ForTranslators: This specifies who is providing this archive. In the case of Debian the
       string will read 'Debian'. Other providers may use their own string, such as
       "Ubuntu" or "Xandros". */
    { "origin", matcher_type_origin },
    { "priority", matcher_type_priority },
    { "provides", matcher_type_provides },
    { "section", matcher_type_section },
    { "source-package", matcher_type_source_package },
    { "source-version", matcher_type_source_version },
    { "tag", matcher_type_tag },
    { "task", matcher_type_task },
    { "true", matcher_type_true },
    { "upgradable", matcher_type_upgradable },
    { "user-tag", matcher_type_user_tag },
    { "version", matcher_type_version },
    { "virtual", matcher_type_virtual },
    /* ForTranslators: Opposite of narrow. Search for "widen" in this file for details. */
    { "widen", matcher_type_widen }
  };
}

bool pkg_matcher_real::matches(const pkgCache::PkgIterator &pkg,
			       aptitudeDepCache &cache,
			       pkgRecords &records,
			       match_stack &stack)
{
  for(pkgCache::VerIterator v = pkg.VersionList();
      !v.end(); ++v)
    if(matches(pkg, v, cache, records, stack))
      return true;

  if(pkg.VersionList().end())
    return matches(pkg, pkgCache::VerIterator(cache, 0),
		   cache, records, stack);
  else
    return false;
}

pkg_match_result *pkg_matcher_real::get_match(const pkgCache::PkgIterator &pkg,
					      aptitudeDepCache &cache,
					      pkgRecords &records,
					      match_stack &stack)
{
  pkg_match_result *rval = NULL;

  for(pkgCache::VerIterator v = pkg.VersionList();
      rval == NULL && !v.end(); ++v)
    rval = get_match(pkg, v, cache, records, stack);

  if(pkg.VersionList().end())
    rval = get_match(pkg, pkgCache::VerIterator(cache, 0),
		     cache, records, stack);

  return rval;
}

/** A common class to use when there's no interesting result.  This is
 *  distinct from a match failure: an example would be the
 *  auto_matcher.
 */
class empty_match_result : public pkg_match_result
{
public:
  unsigned int num_groups() {return 0;}

  const string &group(unsigned int n) {abort();}
};

class pkg_nonstring_matcher : public pkg_matcher_real
{
public:
  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    return matches(pkg, ver, cache, records, stack) ? new empty_match_result : NULL;
  }
};

class unitary_result : public pkg_match_result
{
  string s;
public:
  unitary_result(const string &_s):s(_s) {}

  unsigned int num_groups() {return 1;}

  const string &group(unsigned int n)
  {
    if(n != 0)
      abort();

    return s;
  }
};

class result_pair : public pkg_match_result
{
  pkg_match_result *r1, *r2;
public:
  result_pair(pkg_match_result *_r1,
	      pkg_match_result *_r2)
    :r1(_r1), r2(_r2)
  {
  }

  ~result_pair()
  {
    delete r1;
    delete r2;
  }

  unsigned int num_groups()
  {
    return r1->num_groups() + r2->num_groups();
  }

  const string &group(unsigned int n)
  {
    unsigned int num_groups_r1 = r1->num_groups();

    if(n < num_groups_r1)
      return r1->group(n);
    else
      return r2->group(n - num_groups_r1);
  }
};

class pkg_string_matcher : public pkg_matcher_real
{
  regex_t pattern_nogroup;
  regex_t pattern_group;
  bool pattern_nogroup_initialized:1;
  bool pattern_group_initialized:1;

  pkg_string_matcher()
  {
  }

  void do_compile(const string &_pattern,
		  regex_t &pattern,
		  int cflags)
  {
    int err=regcomp(&pattern, _pattern.c_str(), cflags);
    if(err!=0)
      {
	size_t needed=regerror(err, &pattern, NULL, 0);

	auto_ptr<char> buf(new char[needed+1]);

	regerror(err, &pattern, buf.get(), needed+1);

	throw CompilationException("Regex compilation error: %s", buf.get());
      }
  }

  void compile(const string &_pattern)
  {
    do_compile(_pattern, pattern_nogroup, REG_ICASE|REG_EXTENDED|REG_NOSUB);
    pattern_nogroup_initialized=true;

    do_compile(_pattern, pattern_group, REG_ICASE|REG_EXTENDED);
    pattern_group_initialized=true;
  }

public:
  class match_result : public pkg_match_result
  {
    // well, it's pretty much necessary to copy all the groups anyway
    // :(
    vector<string> matches;
  public:
    match_result(const char *s, regmatch_t *pmatch, int matches_len)
    {
      for(int i=0; i<matches_len && pmatch[i].rm_so>=0; ++i)
	{
	  matches.push_back(string());
	  matches.back().assign(s, pmatch[i].rm_so,
				pmatch[i].rm_eo-pmatch[i].rm_so);
	}
    }

    unsigned int num_groups() {return matches.size();}
    const string &group(unsigned int n) {return matches[n];}
  };

  pkg_string_matcher (const string &_pattern)
    :pattern_nogroup_initialized(false),
     pattern_group_initialized(false)
  {
    // By convention, empty patterns match anything. (anything, you
    // hear me??)  That allows you to put "~m" into the pattern
    // grouping policy and get a by-maintainer grouping out.
    if(_pattern.empty())
      compile(".*");
    else
      compile(_pattern);
  }

  ~pkg_string_matcher()
  {
    if(pattern_nogroup_initialized)
      regfree(&pattern_nogroup);
    if(pattern_group_initialized)
      regfree(&pattern_group);
  }

  bool string_matches(const char *s)
  {
    return !regexec(&pattern_nogroup, s, 0, NULL, 0);
  }

  match_result *get_string_match(const char *s)
  {
    // ew.  You need a hard limit here.
    regmatch_t matches[30];

    bool matched = (regexec(&pattern_group, s,
			    sizeof(matches)/sizeof(regmatch_t),
			    matches, 0) == 0);

    if(!matched)
      return NULL;
    else
      return new match_result(s, matches,
			      sizeof(matches)/sizeof(regmatch_t));
  }
};

typedef pair<bool, std::string> match_target;

class pkg_trivial_string_matcher : public pkg_string_matcher
{
public:
  pkg_trivial_string_matcher (const string &s) : pkg_string_matcher(s)
  {
  }

  // If the first element is false, the match fails; otherwise, it
  // proceeds using the second element.
  virtual match_target val(const pkgCache::PkgIterator &pkg,
			   const pkgCache::VerIterator &ver,
			   aptitudeDepCache &cache,
			   pkgRecords &records)=0;

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    match_target v = val(pkg, ver, cache, records);

    if(!v.first)
      return false;
    else
      return string_matches(v.second.c_str());
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    match_target v = val(pkg, ver, cache, records);

    if(!v.first)
      return NULL;
    else
      return get_string_match(v.second.c_str());
  }
};

class pkg_name_matcher:public pkg_trivial_string_matcher
{
public:
  pkg_name_matcher(const string &s):pkg_trivial_string_matcher(s) {}

  match_target val(const pkgCache::PkgIterator &pkg,
		   const pkgCache::VerIterator &ver,
		   aptitudeDepCache &cache,
		   pkgRecords &records)
  {
    return match_target(true, pkg.Name());
  }
};

class pkg_description_matcher:public pkg_trivial_string_matcher
{
public:
  pkg_description_matcher(const string &s):pkg_trivial_string_matcher(s) {}

  match_target val(const pkgCache::PkgIterator &pkg,
		   const pkgCache::VerIterator &ver,
		   aptitudeDepCache &cache,
		   pkgRecords &records)
  {
    if(ver.end())
      return match_target(false, "");
    else
      return match_target(true, cw::util::transcode(get_long_description(ver, &records).c_str()));
  }
};

class pkg_maintainer_matcher : public pkg_trivial_string_matcher
{
public:
  pkg_maintainer_matcher(const string &s):pkg_trivial_string_matcher(s)
  {
  }

  match_target val(const pkgCache::PkgIterator &pkg,
		   const pkgCache::VerIterator &ver,
		   aptitudeDepCache &cache,
		   pkgRecords &records)
  {
    if(ver.end())
      return match_target(false, "");
    else
      return match_target(true, records.Lookup(ver.FileList()).Maintainer().c_str());
  }
};

class pkg_section_matcher : public pkg_trivial_string_matcher
{
public:
  pkg_section_matcher(const string &s):pkg_trivial_string_matcher(s)
  {
  }

  match_target val(const pkgCache::PkgIterator &pkg,
		   const pkgCache::VerIterator &ver,
		   aptitudeDepCache &cache,
		   pkgRecords &records)
  {
    if(ver.end())
      return match_target(false, "");

    const char *s=ver.Section();

    if(!s)
      return match_target(false, "");

    return match_target(true, s);
  }
};

class pkg_version_matcher : public pkg_trivial_string_matcher
{
public:
  pkg_version_matcher(const string &s) : pkg_trivial_string_matcher(s)
  {
  }

  match_target val(const pkgCache::PkgIterator &pkg,
		   const pkgCache::VerIterator &ver,
		   aptitudeDepCache &cache,
		   pkgRecords &records)
  {
    if(ver.end())
      return match_target(false, "");

    const char *s=ver.VerStr();

    if(!s)
      return match_target(false, "");

    return match_target(true, s);
  }
};

// NOTE: pkg_current_version_matcher, pkg_inst_version_matcher, and
// pkg_cand_version_matcher are all a bit inefficient since they loop
// over all versions when they only match one; if they become a
// performance problem (unlikely), you could (carefully!!) implement
// the version-agnostic match variants to speed things up.
class pkg_curr_version_matcher : public pkg_matcher_real
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return !ver.end() && ver == pkg.CurrentVer();
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(matches(pkg, ver, cache, records, stack))
      return new unitary_result(ver.VerStr());
    else
      return NULL;
  }
};

class pkg_cand_version_matcher : public pkg_matcher_real
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return !ver.end() &&
      ver == cache[pkg].CandidateVerIter(cache);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(matches(pkg, ver, cache, records, stack))
      return new unitary_result(ver.VerStr());
    else
      return NULL;
  }
};

class pkg_inst_version_matcher : public pkg_matcher_real
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return !ver.end() &&
      ver == cache[pkg].InstVerIter(cache);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(matches(pkg, ver, cache, records, stack))
      return new unitary_result(ver.VerStr());
    else
      return NULL;
  }
};

pkg_matcher_real *make_package_version_matcher(const string &substr)
{
  if(substr == "CURRENT")
    return new pkg_curr_version_matcher;
  else if(substr == "TARGET")
    return new pkg_inst_version_matcher;
  else if(substr == "CANDIDATE")
    return new pkg_cand_version_matcher;
  else
    return new pkg_version_matcher(substr);
}

class pkg_task_matcher : public pkg_string_matcher
{
public:
  pkg_task_matcher(const string &s) : pkg_string_matcher(s)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &v,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    set<string> *l = get_tasks(pkg);

    if(!l)
      return false;

    for(set<string>::iterator i=l->begin();
	i!=l->end();
	++i)
      if(string_matches(i->c_str()))
	return true;

    return false;
  }

  // Uses the fact that the result returns NULL <=> nothing matched
  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    set<string> *l=get_tasks(pkg);

    if(!l)
      return NULL;

    for(set<string>::iterator i=l->begin();
	i!=l->end();
	++i)
      {
	pkg_match_result *r=get_string_match(i->c_str());

	if(r != NULL)
	  return r;
      }

    return NULL;
  }
};

class pkg_tag_matcher : public pkg_string_matcher
{
public:
  pkg_tag_matcher(const string &s)
    : pkg_string_matcher(s)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
#ifdef HAVE_EPT
    typedef ept::debtags::Tag tag;
    using aptitude::apt::get_tags;
#endif

#ifdef HAVE_EPT
    const std::set<tag> realTags(get_tags(pkg));
    const std::set<tag> * const tags(&realTags);
#else
    const std::set<tag> * const tags(get_tags(pkg));
#endif

    if(tags == NULL)
      return false;

    for(std::set<tag>::const_iterator i=tags->begin(); i!=tags->end(); ++i)
      {
#ifdef HAVE_EPT
	std::string name(i->fullname());
#else
	const std::string name = i->str().c_str();
#endif
	if(string_matches(name.c_str()))
	  return true;
      }

    return false;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
#ifdef HAVE_EPT
    typedef ept::debtags::Tag tag;
    using aptitude::apt::get_tags;
#endif

#ifdef HAVE_EPT
    const set<tag> realTags(get_tags(pkg));
    const set<tag> * const tags(&realTags);
#else
    const set<tag> * const tags(get_tags(pkg));
#endif

    if(tags == NULL)
      return NULL;

    for(set<tag>::const_iterator i=tags->begin(); i!=tags->end(); ++i)
      {
#ifdef HAVE_EPT
	std::string name(i->fullname());
#else
	const std::string name = i->str().c_str();
#endif

	pkg_match_result *res = get_string_match(name.c_str());
	if(res != NULL)
	  return res;
      }

    return NULL;
  }
};

class pkg_user_tag_matcher : public pkg_string_matcher
{
  std::map<aptitudeDepCache::user_tag,
	   match_result *> cached_matches;

  match_result *noncopy_get_match(const pkgCache::PkgIterator &pkg,
				  aptitudeDepCache &cache)
  {
    const std::set<aptitudeDepCache::user_tag> &user_tags =
      cache.get_ext_state(pkg).user_tags;

    for(std::set<aptitudeDepCache::user_tag>::const_iterator it =
	  user_tags.begin(); it != user_tags.end(); ++it)
      {
	std::map<aptitudeDepCache::user_tag, match_result *>::const_iterator
	  found = cached_matches.find(*it);

	if(found != cached_matches.end() && found->second != NULL)
	  return found->second;

	match_result *result = get_string_match(cache.deref_user_tag(*it).c_str());
	cached_matches[*it] = result;
	if(result != NULL)
	  return result;
      }

    return NULL;
  }
public:
  pkg_user_tag_matcher(const std::string &s)
    : pkg_string_matcher(s)
    
  {
  }

  ~pkg_user_tag_matcher()
  {
    for(std::map<aptitudeDepCache::user_tag, match_result *>::const_iterator it
	  = cached_matches.begin(); it != cached_matches.end(); ++it)
      delete it->second;
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return noncopy_get_match(pkg, cache) != NULL;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    match_result *rval = noncopy_get_match(pkg, cache);
    if(rval == NULL)
      return NULL;
    else
      return new match_result(*rval);
  }
};

//  Package-file info matchers.  Match a package if any of its
// available files (for all versions) match the given criteria.
//
//  Should I use templates?
class pkg_origin_matcher : public pkg_string_matcher
{
public:
  pkg_origin_matcher(const string &s) : pkg_string_matcher(s)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(ver.end())
      return false;

    for(pkgCache::VerFileIterator f = ver.FileList(); !f.end(); ++f)
      {
	pkgCache::PkgFileIterator cur = f.File();

	if(!cur.end() && cur.Origin() && string_matches(cur.Origin()))
	  return true;
      }

    return false;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(ver.end())
      return NULL;

    for(pkgCache::VerFileIterator f = ver.FileList(); !f.end(); ++f)
      {
	pkgCache::PkgFileIterator cur = f.File();

	if(!cur.end() && cur.Origin())
	  {
	    pkg_match_result *r = get_string_match(cur.Origin());

	    if(r != NULL)
	      return r;
	  }
      }

    return NULL;
  }
};

class pkg_archive_matcher : public pkg_string_matcher
{
public:
  pkg_archive_matcher(const string &s) : pkg_string_matcher(s)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(ver.end() || ver.FileList().end())
      return false;

    for(pkgCache::VerFileIterator f = ver.FileList(); !f.end(); ++f)
      {
	pkgCache::PkgFileIterator cur = f.File();

	if(!cur.end() && cur.Archive() && string_matches(cur.Archive()))
	  return true;
      }

    return false;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(ver.end() || ver.FileList().end())
      return NULL;

    for(pkgCache::VerFileIterator f = ver.FileList(); !f.end(); ++f)
      {
	pkgCache::PkgFileIterator cur = f.File();

	if(!cur.end() && cur.Archive())
	  {
	    pkg_match_result *r = get_string_match(cur.Archive());

	    if(r != NULL)
	      return r;
	  }
      }

    return NULL;
  }
};

class pkg_source_package_matcher : public pkg_string_matcher
{
public:
  pkg_source_package_matcher(const string &s) : pkg_string_matcher(s)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(ver.end() || ver.FileList().end())
      return false;

    bool checked_real_package = false;
    for(pkgCache::VerFileIterator f = ver.FileList(); !f.end(); ++f)
      {
	pkgRecords::Parser &p = records.Lookup(f);

	if(p.SourcePkg().empty())
	  {
	    if(!checked_real_package)
	      {
		if(string_matches(pkg.Name()))
		  return true;
	      }
	  }
	else if(string_matches(p.SourcePkg().c_str()))
	  return true;
      }

    return false;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(ver.end() || ver.FileList().end())
      return false;

    bool checked_real_package = false;
    for(pkgCache::VerFileIterator f = ver.FileList(); !f.end(); ++f)
      {
	pkgRecords::Parser &p = records.Lookup(f);

	if(p.SourcePkg().empty())
	  {
	    if(!checked_real_package)
	      {
		pkg_match_result *r = get_string_match(pkg.Name());

		if(r != NULL)
		  return r;
	      }
	  }
	else
	  {
	    pkg_match_result *r = get_string_match(p.SourcePkg().c_str());

	    if(r != NULL)
	      return r;
	  }
      }

    return NULL;
  }
};

class pkg_source_version_matcher : public pkg_string_matcher
{
public:
  pkg_source_version_matcher(const string &s) : pkg_string_matcher(s)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(ver.end() || ver.FileList().end())
      return false;

    bool checked_real_package = false;
    for(pkgCache::VerFileIterator f = ver.FileList(); !f.end(); ++f)
      {
	pkgRecords::Parser &p = records.Lookup(f);

	if(p.SourceVer().empty())
	  {
	    if(!checked_real_package)
	      {
		if(string_matches(ver.VerStr()))
		  return true;
	      }
	  }
	else if(string_matches(p.SourceVer().c_str()))
	  return true;
      }

    return false;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(ver.end() || ver.FileList().end())
      return false;

    bool checked_real_package = false;
    for(pkgCache::VerFileIterator f = ver.FileList(); !f.end(); ++f)
      {
	pkgRecords::Parser &p = records.Lookup(f);

	if(p.SourceVer().empty())
	  {
	    if(!checked_real_package)
	      {
		pkg_match_result *r = get_string_match(ver.VerStr());

		if(r != NULL)
		  return r;
	      }
	  }
	else
	  {
	    pkg_match_result *r = get_string_match(p.SourceVer().c_str());

	    if(r != NULL)
	      return r;
	  }
      }

    return NULL;
  }
};

class pkg_auto_matcher:public pkg_matcher_real
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return
      (!pkg.CurrentVer().end() || cache[pkg].Install()) &&
      (cache[pkg].Flags & pkgCache::Flag::Auto);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    return matches(pkg, ver, cache, records, stack)
      ? new unitary_result(_("Automatically Installed")) : NULL;
  }
};

class pkg_broken_matcher:public pkg_matcher_real
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(ver.end())
      return false;
    else
      {
	aptitudeDepCache::StateCache &state = cache[pkg];
	return state.NowBroken() || state.InstBroken();
      }
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    return matches(pkg, ver, cache, records, stack) ? new unitary_result(_("Broken")) : NULL;
  }
};

class pkg_priority_matcher:public pkg_matcher_real
{
  pkgCache::State::VerPriority type;
public:
  pkg_priority_matcher(pkgCache::State::VerPriority _type)
    :type(_type) {}

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(ver.end())
      return false;
    else
      return ver->Priority == type;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(ver.end())
      return NULL;
    else if(ver->Priority != type)
      return NULL;
    else
      return new unitary_result(const_cast<pkgCache::VerIterator &>(ver).PriorityType());
  }
};

pkg_match_result *dep_match(const pkgCache::DepIterator &dep)
{
  string realization;

  pkgCache::DepIterator start, end;

  surrounding_or(dep, start, end);

  bool first = true;

  while(start != end)
    {
      if(!first)
	realization += " | ";

      first = false;

      realization += start.TargetPkg().Name();

      if(start.TargetVer())
	{
	  realization += " (";
	  realization += start.CompType();
	  realization += " ";
	  realization += start.TargetVer();
	  realization += ")";
	}

      ++start;
    }

  // erm...
  return new result_pair(new unitary_result(const_cast<pkgCache::DepIterator &>(dep).DepType()),
			 new unitary_result(realization));
}

// Matches packages with unmet dependencies of a particular type.
class pkg_broken_type_matcher:public pkg_matcher_real
{
  pkgCache::Dep::DepType type; // Which type to match
public:
  pkg_broken_type_matcher(pkgCache::Dep::DepType _type)
    :type(_type) {}

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(ver.end())
      return false;
    else
      {
	pkgCache::DepIterator dep=ver.DependsList();

	while(!dep.end())
	  {
	    // Skip to the end of the Or group to check GInstall
	    while(dep->CompareOp & pkgCache::Dep::Or)
	      ++dep;

	    if(dep->Type==type &&
	       !(cache[dep] & pkgDepCache::DepGInstall))
	      // Oops, it's broken..
	      return true;

	    ++dep;
	  }
      }
    return false;
  }

  pkg_match_result * get_match(const pkgCache::PkgIterator &pkg,
			       const pkgCache::VerIterator &ver,
			       aptitudeDepCache &cache,
			       pkgRecords &records,
			       match_stack &stack)
  {
    if(ver.end())
      return NULL;
    else
      {
	pkgCache::DepIterator dep=ver.DependsList();

	while(!dep.end())
	  {
	    // Skip to the end of the Or group to check GInstall
	    while(dep->CompareOp & pkgCache::Dep::Or)
	      ++dep;

	    if(dep->Type==type &&
	       !(cache[dep] & pkgDepCache::DepGInstall))
	      // Oops, it's broken..
	      return dep_match(dep);

	    ++dep;
	  }
      }

    return NULL;
  }
};

// This matches packages based on the action that will be taken with them.
//
// It will treat a request for a non-auto type as also being a request
// for the auto type.
class pkg_action_matcher:public pkg_matcher_real
{
  pkg_action_state type;
  bool require_purge;
public:
  pkg_action_matcher(pkg_action_state _type, bool _require_purge)
    :type(_type), require_purge(_require_purge)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(require_purge &&
       (cache[pkg].iFlags & pkgDepCache::Purge) == 0)
      return false;
    else
      {
	switch(type)
	  {
	  case pkg_install:
	    {
	      pkg_action_state thetype = find_pkg_state(pkg, cache);
	      return thetype==pkg_install || thetype==pkg_auto_install;
	    }
	  case pkg_hold:
	    return !pkg.CurrentVer().end() && cache.get_ext_state(pkg).selection_state == pkgCache::State::Hold;
	  case pkg_remove:
	    {
	      pkg_action_state thetype = find_pkg_state(pkg, cache);

	      return thetype==pkg_remove || thetype==pkg_auto_remove ||
		thetype==pkg_unused_remove;
	    }
	  default:
	    {
	      pkg_action_state thetype = find_pkg_state(pkg, cache);

	      return thetype==type;
	    }
	  }
      }
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(!matches(pkg, ver, cache, records, stack))
      return NULL;
    else
      switch(type)
	{
	case pkg_unchanged: // shouldn't happen (?)
	  return new unitary_result(_("Unchanged"));
	case pkg_broken:
	  return new unitary_result(_("Broken"));
	case pkg_unused_remove:
	  return new unitary_result(_("Remove [unused]"));
	case pkg_auto_hold:
	  return new unitary_result(_("Hold [auto]"));
	case pkg_auto_install:
	  return new unitary_result(_("Install [auto]"));
	case pkg_auto_remove:
	  return new unitary_result(_("Remove [auto]"));
	case pkg_downgrade:
	  return new unitary_result(_("Downgrade"));
	case pkg_hold:
	  return new unitary_result(_("Hold"));
	case pkg_reinstall:
	  return new unitary_result(_("Reinstall"));
	case pkg_install:
	  return new unitary_result(_("Install"));
	case pkg_remove:
	  return new unitary_result(_("Remove"));
	case pkg_upgrade:
	  return new unitary_result(_("Upgrade"));
	default:
	  // should never happen.
	  abort();
	}
  }
};

class pkg_keep_matcher:public pkg_matcher_real
{
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return cache[pkg].Keep();
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(cache[pkg].Keep())
      return new unitary_result(_("Keep"));
    else
      return NULL;
  }
};

/** Matches package versions that are not associated with a 'real'
 *  package.  Applied to a whole package, this matches virtual
 *  packages; it also matches package versions corresponding to
 *  removing a package.
 */
class pkg_virtual_matcher:public pkg_matcher_real
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return ver.end();
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(!ver.end())
      return NULL;
    else
      return new unitary_result(_("Virtual"));
  }
};

/** Matches the currently installed version of a package.
 */
class pkg_installed_matcher:public pkg_matcher_real
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return !pkg.CurrentVer().end() && ver == pkg.CurrentVer();
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(pkg.CurrentVer().end() || ver != pkg.CurrentVer())
      return NULL;
    else
      return new unitary_result(_("Installed"));
  }
};

class pkg_essential_matcher:public pkg_matcher_real
// Matches essential packages
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return
      (pkg->Flags&pkgCache::Flag::Essential)==pkgCache::Flag::Essential ||
      (pkg->Flags&pkgCache::Flag::Important)==pkgCache::Flag::Important;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(!matches(pkg, ver, cache, records, stack))
      return NULL;
    else
      return new unitary_result(_("Essential"));
  }
};

class pkg_configfiles_matcher:public pkg_matcher_real
// Matches a package which was removed but has config files remaining
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return pkg->CurrentState==pkgCache::State::ConfigFiles;
  }

  // ???
  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(pkg->CurrentState == pkgCache::State::ConfigFiles)
      return new unitary_result(_("Config Files Remain"));
    else
      return NULL;
  }
};

// Matches packages with a dependency on the given pattern.
class pkg_dep_matcher:public pkg_matcher_real
{
private:
  pkg_matcher_real *pattern;
  pkgCache::Dep::DepType type;

  /** If \b true, only broken dependencies will be matched. */
  bool broken;

public:
  pkg_dep_matcher(pkgCache::Dep::DepType _type,
		  pkg_matcher_real *_pattern,
		  bool _broken)
    :pattern(_pattern), type(_type), broken(_broken)
  {
  }
  ~pkg_dep_matcher() {delete pattern;}

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    eassert(!pkg.end());
    if(ver.end())
      return false;

    for(pkgCache::DepIterator dep=ver.DependsList(); !dep.end(); ++dep)
      {
	if( (type == dep->Type) ||
	    (type == pkgCache::Dep::Depends &&
	     dep->Type == pkgCache::Dep::PreDepends))
	  {
	    if(broken)
	      {
		pkgCache::DepIterator d2(cache, &*dep);
		while(d2->CompareOp & pkgCache::Dep::Or)
		  ++d2;
		if(cache[d2] & pkgDepCache::DepGInstall)
		  continue;
	      }

	    // See if a versionless match works,.
	    if(dep.TargetPkg().VersionList().end() &&
	       pattern->matches(dep.TargetPkg(), dep.TargetPkg().VersionList(), cache, records, stack))
	      return true;

	    for(pkgCache::VerIterator i=dep.TargetPkg().VersionList(); !i.end(); i++)
	      if(_system->VS->CheckDep(i.VerStr(), dep->CompareOp, dep.TargetVer()))
		{
		  if(pattern->matches(dep.TargetPkg(), i, cache, records, stack))
		    return true;
		}
	  }
      }

    return false;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    eassert(!pkg.end());
    if(ver.end())
      return NULL;

    for(pkgCache::DepIterator dep=ver.DependsList(); !dep.end(); ++dep)
      {
	if( (type == dep->Type) ||
	    (type == pkgCache::Dep::Depends &&
	     dep->Type == pkgCache::Dep::PreDepends))
	  {
	    if(broken)
	      {
		pkgCache::DepIterator d2(cache, &*dep);
		while(d2->CompareOp & pkgCache::Dep::Or)
		  ++d2;
		if(cache[d2] & pkgDepCache::DepGInstall)
		  continue;
	      }

	    // See if a versionless match works,.
	    if(dep.TargetPkg().VersionList().end())
	      {
		pkg_match_result *r = pattern->get_match(dep.TargetPkg(), dep.TargetPkg().VersionList(), cache, records, stack);

		if(r)
		  return new result_pair(r, dep_match(dep));
	      }

	    for(pkgCache::VerIterator i=dep.TargetPkg().VersionList(); !i.end(); i++)
	      if(_system->VS->CheckDep(i.VerStr(), dep->CompareOp, dep.TargetVer()))
		{
		  pkg_match_result *r = pattern->get_match(dep.TargetPkg(), i, cache, records, stack);

		  if(r)
		    return new result_pair(r, dep_match(dep));
		}
	  }
      }

    return false;
  }
};

class pkg_or_matcher:public pkg_matcher_real
{
  pkg_matcher_real *left,*right;
public:
  pkg_or_matcher(pkg_matcher_real *_left, pkg_matcher_real *_right)
    :left(_left),right(_right)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return left->matches(pkg, ver, cache, records, stack) ||
      right->matches(pkg, ver, cache, records, stack);
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return left->matches(pkg, cache, records, stack) ||
      right->matches(pkg, cache, records, stack);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    pkg_match_result *lr = left->get_match(pkg, ver, cache, records, stack);

    if(lr != NULL)
      return lr;
    else
      return right->get_match(pkg, ver, cache, records, stack);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    pkg_match_result *lr = left->get_match(pkg, cache, records, stack);

    if(lr != NULL)
      return lr;
    else
      return right->get_match(pkg, cache, records, stack);
  }

  ~pkg_or_matcher()
  {
    delete left;
    delete right;
  }
};

class pkg_and_matcher:public pkg_matcher_real
{
  pkg_matcher_real *left,*right;
public:
  pkg_and_matcher(pkg_matcher_real *_left, pkg_matcher_real *_right)
    :left(_left),right(_right)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return left->matches(pkg, ver, cache, records, stack) &&
      right->matches(pkg, ver, cache, records, stack);
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return left->matches(pkg, cache, records, stack) &&
      right->matches(pkg, cache, records, stack);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    pkg_match_result *r1 = left->get_match(pkg, ver, cache, records, stack);

    if(r1 == NULL)
      return NULL;

    pkg_match_result *r2 = right->get_match(pkg, ver, cache, records, stack);

    if(r2 == NULL)
      {
	delete r1;
	return NULL;
      }

    return new result_pair(r1, r2);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    pkg_match_result *r1 = left->get_match(pkg, cache, records, stack);

    if(r1 == NULL)
      return NULL;

    pkg_match_result *r2 = right->get_match(pkg, cache, records, stack);

    if(r2 == NULL)
      {
	delete r1;
	return NULL;
      }

    return new result_pair(r1, r2);
  }

  ~pkg_and_matcher()
  {
    delete left;
    delete right;
  }
};

class pkg_not_matcher:public pkg_matcher_real
{
  pkg_matcher_real *child;
public:
  pkg_not_matcher(pkg_matcher_real *_child)
    :child(_child)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return !child->matches(pkg, ver, cache, records, stack);
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return !child->matches(pkg, cache, records, stack);
  }

  // Eh, there isn't really a good choice about what to return here...
  // just return an empty result if the child doesn't match.
  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    pkg_match_result *child_match = child->get_match(pkg, ver, cache, records, stack);

    if(child_match == NULL)
      return new empty_match_result;
    else
      {
	delete child_match;
	return NULL;
      }
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    pkg_match_result *child_match = child->get_match(pkg, cache, records, stack);

    if(child_match == NULL)
      return new empty_match_result;
    else
      {
	delete child_match;
	return NULL;
      }
  }

  ~pkg_not_matcher() {delete child;}
};

/** Widen the search to include all versions of every package. */
class pkg_widen_matcher : public pkg_matcher_real
{
  pkg_matcher_real *pattern;
public:
  pkg_widen_matcher(pkg_matcher_real *_pattern)
    : pattern(_pattern)
  {
  }

  ~pkg_widen_matcher()
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return pattern->matches(pkg, cache, records, stack);
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return pattern->matches(pkg, cache, records, stack);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    return pattern->get_match(pkg, cache, records, stack);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    return pattern->get_match(pkg, cache, records, stack);
  }
};

/** Narrow the search to versions that match a pattern. */
class pkg_select_matcher : public pkg_matcher_real
{
  pkg_matcher_real *filter;
  pkg_matcher_real *pattern;
public:
  pkg_select_matcher(pkg_matcher_real *_filter,
		     pkg_matcher_real *_pattern)
    : filter(_filter), pattern(_pattern)
  {
  }

  ~pkg_select_matcher()
  {
    delete filter;
    delete pattern;
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return filter->matches(pkg, ver, cache, records, stack) &&
      pattern->matches(pkg, ver, cache, records, stack);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(filter->matches(pkg, ver, cache, records, stack))
      return pattern->get_match(pkg, ver, cache, records, stack);
    else
      return NULL;
  }
};

// Matches packages that were garbage-collected.
class pkg_garbage_matcher:public pkg_matcher_real
{
public:
  pkg_garbage_matcher() {}

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(ver.end())
      return false;
    else
      return cache[pkg].Garbage;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(!matches(pkg, ver, cache, records, stack))
      return NULL;
    else
      return new unitary_result(_("Garbage"));
  }
};

// A dummy matcher that matches any package.
class pkg_true_matcher:public pkg_matcher_real
{
public:
  pkg_true_matcher() {}

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return true;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    return new empty_match_result;
  }
};

// A dummy matcher that matches no packages.
class pkg_false_matcher:public pkg_matcher_real
{
public:
  pkg_false_matcher() {}

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return false;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    return NULL;
  }
};

// Matches packages which have a dependency of the given type declared
// on them by a package matching a given pattern.
//
// Traces through Provided packages as well.
class pkg_revdep_matcher:public pkg_matcher_real
{
  pkgCache::Dep::DepType type;
  pkg_matcher_real *pattern;

  /** If \b true, only install-broken dependencies will cause a
   *  match.
   */
  bool broken;

public:
  pkg_revdep_matcher(pkgCache::Dep::DepType _type,
		     pkg_matcher_real *_pattern,
		     bool _broken)
    :type(_type), pattern(_pattern), broken(_broken)
  {
  }

  ~pkg_revdep_matcher()
  {
    delete pattern;
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    // Check direct dependencies.
    for(pkgCache::DepIterator d=pkg.RevDependsList(); !d.end(); ++d)
      {
	if(broken)
	  {
	    pkgCache::DepIterator d2(cache, &*d);
	    while(d2->CompareOp & pkgCache::Dep::Or)
	      ++d2;
	    if(cache[d2] & pkgDepCache::DepGInstall)
	      continue;
	  }

	if((d->Type==type ||
	    (type==pkgCache::Dep::Depends && d->Type==pkgCache::Dep::PreDepends)) &&
	   (!d.TargetVer() || (!ver.end() &&
			       _system->VS->CheckDep(ver.VerStr(), d->CompareOp, d.TargetVer()))) &&
	   pattern->matches(d.ParentPkg(), d.ParentVer(),
			    cache, records, stack))
	  return true;
      }

    // Check dependencies through virtual packages.  ie, things that Depend
    // on stuff this package [version] Provides.
    if(!ver.end())
      for(pkgCache::PrvIterator p=ver.ProvidesList(); !p.end(); ++p)
	{
	  for(pkgCache::DepIterator d=p.ParentPkg().RevDependsList();
	      !d.end(); ++d)
	    {
	      if(broken)
		{
		  pkgCache::DepIterator d2(cache, &*d);
		  while(d2->CompareOp & pkgCache::Dep::Or)
		    ++d2;
		  if(cache[d2] & pkgDepCache::DepGInstall)
		    continue;
		}

	      // Only unversioned dependencies can match here.
	      if(d->Type==type && !d.TargetVer() &&
		 pattern->matches(d.ParentPkg(), d.ParentVer(),
				  cache, records, stack))
		return true;
	    }
	}

    return false;
  }

  // Too much duplication, can I factor out the common stuff here?
  // C++ doesn't make it easy..
  //
  // Maybe I should just forget trying to be efficient and base
  // everything on match results..
  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    // Check direct dependencies.
    for(pkgCache::DepIterator d=pkg.RevDependsList(); !d.end(); ++d)
      {
	if(broken)
	  {
	    pkgCache::DepIterator d2(cache, &*d);
	    while(d2->CompareOp & pkgCache::Dep::Or)
	      ++d2;
	    if(cache[d2] & pkgDepCache::DepGInstall)
	      continue;
	  }

	if((d->Type==type ||
	    (type==pkgCache::Dep::Depends && d->Type==pkgCache::Dep::PreDepends)) &&
	   (!d.TargetVer() || (!ver.end() &&
			       _system->VS->CheckDep(ver.VerStr(), d->CompareOp, d.TargetVer()))))
	  {
	    pkg_match_result *r = pattern->get_match(d.ParentPkg(),
						     d.ParentVer(),
						     cache, records,
						     stack);

	    if(r != NULL)
	      return new result_pair(r, dep_match(d));
	  }
      }

    // Check dependencies through virtual packages.  ie, things that Depend
    // on stuff this package [version] Provides.
    if(!ver.end())
      for(pkgCache::PrvIterator p=ver.ProvidesList(); !p.end(); ++p)
	{
	  for(pkgCache::DepIterator d=p.ParentPkg().RevDependsList();
	      !d.end(); ++d)
	    {
	      // Only unversioned dependencies can match here.
	      if(d->Type==type && !d.TargetVer())
		{
		  if(broken)
		    {
		      pkgCache::DepIterator d2(cache, &*d);
		      while(d2->CompareOp & pkgCache::Dep::Or)
			++d2;
		      if(cache[d2] & pkgDepCache::DepGInstall)
			continue;
		    }

		  pkg_match_result *r = pattern->get_match(d.ParentPkg(),
							   d.ParentVer(),
							   cache, records,
							   stack);
		  if(r != NULL)
		    return new result_pair(r, dep_match(d));
		}
	    }
	}

    return NULL;
  }
};


/** Matches packages that provide a package that matches the given
 *  pattern.
 */
class pkg_provides_matcher:public pkg_matcher_real
{
  pkg_matcher_real *pattern;
public:
  pkg_provides_matcher(pkg_matcher_real *_pattern)
    :pattern(_pattern)
  {
  }

  ~pkg_provides_matcher() 
  {
    delete pattern;
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(ver.end())
      return false;

    for(pkgCache::PrvIterator p=ver.ProvidesList(); !p.end(); ++p)
      {
	// Assumes no provided version.
	if(pattern->matches(p.ParentPkg(), cache, records, stack))
	  return true;
      }

    return false;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(ver.end())
      return NULL;

    for(pkgCache::PrvIterator p=ver.ProvidesList(); !p.end(); ++p)
      {
	pkg_match_result *r = pattern->get_match(p.ParentPkg(), pkgCache::VerIterator(cache),
						 cache, records, stack);

	if(r != NULL)
	  return new result_pair(r, new unitary_result(_("Provides")));
      }

    return false;
  }
};

/** Matches packages which are provided by a package that fits the
 *  given pattern.
 */
class pkg_revprv_matcher:public pkg_matcher_real
{
  pkg_matcher_real *pattern;
public:
  pkg_revprv_matcher(pkg_matcher_real *_pattern)
    :pattern(_pattern) 
  {
  }

  ~pkg_revprv_matcher() 
  {
    delete pattern;
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    for(pkgCache::PrvIterator p=pkg.ProvidesList(); !p.end(); ++p)
      {
	if(pattern->matches(p.OwnerPkg(), p.OwnerVer(), cache, records, stack))
	  return true;
      }

    return false;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    for(pkgCache::PrvIterator p=pkg.ProvidesList(); !p.end(); ++p)
      {
	pkg_match_result *r = pattern->get_match(p.OwnerPkg(),
						 p.OwnerVer(),
						 cache, records,
						 stack);

	if(r != NULL)
	  return new result_pair(r,
				 new unitary_result(_("Provided by")));
      }

    return false;
  }
};

//  Now back from the dead..it seems some people were actually using it ;-)
//
// Matches (non-virtual) packages which no installed package declares
// an "important" dependency on.
//
// Note that the notion of "importantness" is affected by the current
// settings!
class pkg_norevdep_matcher:public pkg_matcher_real
{
public:
  pkg_norevdep_matcher()
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(ver.end())
      return false;
    else
      {
        pkgCache::DepIterator dep=pkg.RevDependsList();

        while(!dep.end())
          {
            if(cache.GetPolicy().IsImportantDep(dep) &&
               !dep.ParentVer().ParentPkg().CurrentVer().end())
              return false;

            ++dep;
          }

        return true;
      }
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(!matches(pkg, ver, cache, records, stack))
      return NULL;
    else
      return new unitary_result(_("No reverse dependencies"));
  }
};

// Matches (non-virtual) packages which no installed package declares
// a dependency of the given type on.
class pkg_norevdep_type_matcher:public pkg_matcher_real
{
  pkgCache::Dep::DepType type; // Which type to match
public:
  pkg_norevdep_type_matcher(pkgCache::Dep::DepType _type)
    :type(_type)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    if(ver.end())
      return false;
    else
      {
	pkgCache::DepIterator dep=pkg.RevDependsList();

	while(!dep.end())
	  {
	    // Return false if the depender is installed.
	    if(dep->Type==type &&
	       !dep.ParentVer().ParentPkg().CurrentVer().end())
	      return false;

	    ++dep;
	  }
      }
    return true;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(!matches(pkg, ver, cache, records, stack))
      return NULL;
    else
      return new unitary_result(pkgCache::DepType(type));
  }
};

class pkg_new_matcher:public pkg_matcher_real
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    // Don't match virtual packages.
    if(pkg.VersionList().end())
      return false;
    else
      return cache.get_ext_state(pkg).new_package;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(!matches(pkg, ver, cache, records, stack))
      return NULL;
    else
      return new unitary_result(_("New Package"));
  }
};

class pkg_upgradable_matcher:public pkg_matcher_real
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return !pkg.CurrentVer().end() && cache[pkg].Upgradable();
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(!matches(pkg, ver, cache, records, stack))
      return NULL;
    else
      return new unitary_result(_("Upgradable"));
  }
};

class pkg_obsolete_matcher : public pkg_matcher_real
{
public:
  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return pkg_obsolete(pkg);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(!matches(pkg, ver, cache, records, stack))
      return NULL;
    else
      return new unitary_result(_("Obsolete"));
  }
};

class pkg_all_matcher : public pkg_matcher_real
{
  pkg_matcher_real *sub_matcher;
public:
  pkg_all_matcher(pkg_matcher_real *_sub_matcher)
    : sub_matcher(_sub_matcher)
  {
  }

  ~pkg_all_matcher()
  {
    delete sub_matcher;
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return sub_matcher->matches(pkg, ver, cache, records, stack);
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    for(pkgCache::VerIterator ver = pkg.VersionList();
	!ver.end(); ++ver)
      {
	if(!sub_matcher->matches(pkg, ver, cache, records, stack))
	   return false;
      }

    return true;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    return sub_matcher->get_match(pkg, ver, cache, records, stack);
  }

  // This will somewhat arbitrarily return the string associated with
  // the last thing matched.  I don't want to return all the strings
  // since that would make it impossible to reliably select a string
  // later in the search expression.
  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    pkg_match_result *tmp = NULL;

    for(pkgCache::VerIterator ver = pkg.VersionList();
	!ver.end(); ++ver)
      {
	delete tmp;
	tmp = sub_matcher->get_match(pkg, ver, cache, records, stack);
	if(tmp == NULL)
	  return tmp;
      }

    return tmp;
  }
};

class pkg_any_matcher : public pkg_matcher_real
{
  pkg_matcher_real *sub_matcher;
public:
  pkg_any_matcher(pkg_matcher_real *_sub_matcher)
    : sub_matcher(_sub_matcher)
  {
  }

  ~pkg_any_matcher()
  {
    delete sub_matcher;
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return sub_matcher->matches(pkg, ver, cache, records, stack);
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    for(pkgCache::VerIterator ver = pkg.VersionList();
	!ver.end(); ++ver)
      {
	if(sub_matcher->matches(pkg, ver, cache, records, stack))
	   return true;
      }

    return false;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    return sub_matcher->get_match(pkg, ver, cache, records, stack);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    for(pkgCache::VerIterator ver = pkg.VersionList();
	!ver.end(); ++ver)
      {
	pkg_match_result *tmp = sub_matcher->get_match(pkg, ver, cache, records, stack);
	if(tmp != NULL)
	  return tmp;
      }

    return NULL;
  }
};

// A restricted binding operator reminiscent of lambda.  I say
// "restricted" because its argument may only range over packages,
// hence it is not computationally complete.  I think that anyone who
// decides not to implement a lambda calculus when it's a natural
// place to go should explain himself, so here are my reasons:
//
//  (a) it would significantly complicate the interface to this
//      module; the data type accepted by pkg_matcher would probably
//      have to become some sort of disjoint sum type (perhaps making
//      the pkg_matcher a Visitor on values).
//
//  (b) it would raise the possibility of non-terminating searches,
//      which would require complexity at the UI level (searches would
//      have to be run in a background thread, like the resolver, and
//      the user would have to be able to cancel a search that was
//      going nowhere).
//
// It's called an "explicit" matcher because it allows the user to
// explicitly specify which package is the target of a matcher.
class pkg_explicit_matcher : public pkg_matcher_real
{
  pkg_matcher_real *sub_matcher;

  class stack_pusher
  {
    match_stack &stack;

  public:
    stack_pusher(match_stack &_stack, const stack_value &val)
      : stack(_stack)
    {
      stack.push_back(val);
    }

    ~stack_pusher()
    {
      // The size should always be non-zero, but avoid blowing up if
      // it's not.
      if(stack.size() > 0)
	stack.pop_back();
    }
  };

public:
  pkg_explicit_matcher(pkg_matcher_real *_sub_matcher)
    : sub_matcher(_sub_matcher)
  {
  }

  ~pkg_explicit_matcher()
  {
    delete sub_matcher;
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    stack_pusher pusher(stack, stack_value::version(pkg, ver));
    return sub_matcher->matches(pkg, ver, cache, records, stack);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    stack_pusher pusher(stack, stack_value::version(pkg, ver));
    return sub_matcher->get_match(pkg, cache, records, stack);
  }


  bool matches(const pkgCache::PkgIterator &pkg,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    stack_pusher pusher(stack, stack_value::package(pkg));
    return sub_matcher->matches(pkg, cache, records, stack);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    stack_pusher pusher(stack, stack_value::package(pkg));
    return sub_matcher->get_match(pkg, cache, records, stack);
  }
};

/** \brief Bind the first argument of the given matcher.
 *
 *  This returns a matcher that ignores the input value and
 *  instead uses the value stored at the given location on the
 *  stack.  It's more or less equivalent to
 *
 *     lambda x . lambda f . lambda y . f x
 */
class pkg_bind_matcher : public pkg_matcher_real
{
  pkg_matcher_real *sub_matcher;
  match_stack::size_type variable;

  bool matches(aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack) const
  {
    eassert(variable >= 0 && variable < stack.size());
    return stack[variable].visit_matches(sub_matcher, cache, records, stack);
  }

  pkg_match_result *get_match(aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack) const
  {
    eassert(variable >= 0 && variable < stack.size());
    return stack[variable].visit_get_match(sub_matcher, cache, records, stack);
  }


public:
  /** \brief Create a new bind matcher.
   *
   *  \param _sub_matcher  the matcher whose argument is to be bound.
   *  \param _variable     the stack variable (referred to by its De
   *                       Bruijn numeral) that will be bound to the
   *                       sub-matcher's first argument.
   */
  pkg_bind_matcher(pkg_matcher_real *_sub_matcher,
		   int _variable)
    : sub_matcher(_sub_matcher),
      variable(_variable)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return matches(cache, records, stack);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    return get_match(cache, records, stack);
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return matches(cache, records, stack);
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    return get_match(cache, records, stack);
  }
};

/** \brief Match packages that correspond to the entry at the
 *  given stack position.
 *
 * If the value is a package, match any version of that package or no
 * version.  If the value is a version, match just that version.
 */
class pkg_equal_matcher : public pkg_matcher_real
{
  match_stack::size_type variable;

public:
  pkg_equal_matcher(match_stack::size_type _variable)
    : variable(_variable)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    eassert(variable >= 0 && variable < stack.size());
    return stack[variable].is_match_for(stack_value::version(pkg, ver));
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    eassert(variable >= 0 && variable < stack.size());
    return stack[variable].is_match_for(stack_value::version(pkg, ver))
      ? new empty_match_result
      : NULL;
  }
};

// Check for terminators.  Not terribly efficient, but I expect under
// 3 terminators in any interesting usage (more than that and I really
// should force yacc to do my bidding)
bool terminate(const string::const_iterator &start,
	       const string::const_iterator &end,
	       const vector<const char *> &terminators)
{
  for(vector<const char *>::const_iterator i = terminators.begin();
      i != terminators.end(); ++i)
    {
      string::const_iterator j1 = start;
      const char *j2 = *i;
      bool matches = true;

      while(j1 != end && *j2 != 0 && matches)
	{
	  if(*j1 != *j2)
	    matches=false;

	  ++j1;
	  ++j2;
	}

      if(matches)
	return true;
    }

  return false;
}

// Parses a dependency type.  Returns (ick) -1 if the type is not
// recognized.
pkgCache::Dep::DepType parse_deptype(const string &s)
{
  if(!strcasecmp(s.c_str(), "depends"))
    return pkgCache::Dep::Depends;
  if(!strcasecmp(s.c_str(), "predepends"))
    return pkgCache::Dep::PreDepends;
  if(!strcasecmp(s.c_str(), "recommends"))
    return pkgCache::Dep::Recommends;
  else if(!strcasecmp(s.c_str(), "suggests"))
    return pkgCache::Dep::Suggests;
  else if(!strcasecmp(s.c_str(), "conflicts"))
    return pkgCache::Dep::Conflicts;
  else if(!strcasecmp(s.c_str(), "breaks"))
    return pkgCache::Dep::DpkgBreaks;
  else if(!strcasecmp(s.c_str(), "replaces"))
    return pkgCache::Dep::Replaces;
  else // ewww.
    return (pkgCache::Dep::DepType) -1;
}

// Ideally this would parse the string and return an action type, but
// purging isn't a separate type to the matcher.  Maybe instead there
// should be a separate enum for the action matcher's modes?
pkg_matcher_real *make_action_matcher(const std::string &action_str)
{
  // Match packages to be installed
  if(!strcasecmp(action_str.c_str(), "install"))
    return new pkg_action_matcher(pkg_install, false);

  // Match packages to be upgraded
  else if(!strcasecmp(action_str.c_str(), "upgrade"))
    return new pkg_action_matcher(pkg_upgrade, false);

  else if(!strcasecmp(action_str.c_str(), "downgrade"))
    return new pkg_action_matcher(pkg_downgrade, false);

  // Match packages to be removed OR purged
  else if(!strcasecmp(action_str.c_str(), "remove"))
    return new pkg_action_matcher(pkg_remove, false);

  // Match packages to be purged
  else if(!strcasecmp(action_str.c_str(), "purge"))
    return new pkg_action_matcher(pkg_remove, true);

  // Match packages to be reinstalled
  else if(!strcasecmp(action_str.c_str(), "reinstall"))
    return new pkg_action_matcher(pkg_reinstall, false);

  // Match held packages
  else if(!strcasecmp(action_str.c_str(), "hold"))
    return new pkg_action_matcher(pkg_hold, false);
  else if(!strcasecmp(action_str.c_str(), "keep"))
    return new pkg_keep_matcher;

  else
    throw CompilationException(_("Unknown action type: %s"),
			       action_str.c_str());
}


static
std::string parse_literal_string_tail(string::const_iterator &start,
				      const string::const_iterator end)
{
  std::string rval;

  while(start != end && *start != '"')
    {
      if(*start == '\\')
	{
	  ++start;
	  if(start != end)
	    {
	      switch(*start)
		{
		case 'n':
		  rval += '\n';
		  break;
		case 't':
		  rval += '\t';
		  break;
		default:
		  rval += *start;
		  break;
		}
	      ++start;
	    }
	}
      else
	{
	  rval += *start;
	  ++start;
	}
    }

  if(start == end)
    throw CompilationException(_("Unterminated literal string after %s"), rval.c_str());

  eassert(*start == '"');
  ++start;

  return rval;
}

// Returns a substring up to the first metacharacter, including escaped
// metacharacters (parentheses, ~, |, and !)
//
// Advances loc to the first character of 's' following the escaped string.
std::string parse_substr(string::const_iterator &start,
			 const string::const_iterator &end,
			 const vector<const char *> &terminators,
			 bool whitespace_breaks)
{
  std::string rval;
  bool done=false;

  // Strip leading whitespace.
  while(start != end && isspace(*start))
    ++start;

  do
    {
      while(start != end &&
	    *start != '(' &&
	    *start != ')' &&
	    *start != '!' &&
	    *start != '~' &&
	    *start != '|' &&
	    *start != '"' &&
	    (!whitespace_breaks || !isspace(*start)) &&
	    !terminate(start, end, terminators))
	{
	  rval += *start;
	  ++start;
	}

      if(start != end && *start == '"')
	{
	  ++start;

	  rval += parse_literal_string_tail(start, end);
	}

      // We quit because we ran off the end of the string or saw a
      // metacharacter.  If the latter case and it was a tilde-escape,
      // add the escaped character to the string and continue.
      if(start != end && start+1 != end && *start == '~')
	{
	  const char next = *(start+1);

	  if(next == '(' || next == ')' ||
	     next == '!' || next == '~' ||
	     next == '|' || next == '"' ||
	     (whitespace_breaks && isspace(next)))
	    {
	      rval += next;
	      start += 2;
	    }
	  else
	    done = true;
	}
      else
	done = true;
    } while(!done);

  return rval;
}

pkgCache::State::VerPriority parse_priority(const string &substr)
{
  const char *s=substr.c_str();

  if(strcasecmp(s, "important") == 0 ||
     (apt_cache_file &&
      strcasecmp(s, (*apt_cache_file)->GetCache().Priority(pkgCache::State::Important)) == 0))
    return pkgCache::State::Important;
  else if(strcasecmp(s, "required") == 0 ||
	  (apt_cache_file &&
	   strcasecmp(s, (*apt_cache_file)->GetCache().Priority(pkgCache::State::Required)) == 0))
    return pkgCache::State::Required;
  else if(strcasecmp(s, "standard") == 0 ||
	  (apt_cache_file &&
	   strcasecmp(s, (*apt_cache_file)->GetCache().Priority(pkgCache::State::Standard)) == 0))
    return pkgCache::State::Standard;
  else if(strcasecmp(s, "optional") == 0 ||
	  (apt_cache_file &&
	   strcasecmp(s, (*apt_cache_file)->GetCache().Priority(pkgCache::State::Optional)) == 0))
    return pkgCache::State::Optional;
  else if(strcasecmp(s, "extra") == 0 ||
	  (apt_cache_file &&
	   strcasecmp(s, (*apt_cache_file)->GetCache().Priority(pkgCache::State::Extra)) == 0))
    return pkgCache::State::Extra;
  else
    throw CompilationException(_("Unknown priority %s"),
			       substr.c_str());
}

void parse_whitespace(string::const_iterator &start,
		      const string::const_iterator &end)
{
  while(start != end && isspace(*start))
    ++start;
}

void parse_required_character(string::const_iterator &start,
			      const string::const_iterator &end,
			      char c)
{
  while(start != end && isspace(*start))
    ++start;

  if(start == end)
    throw CompilationException(_("Match pattern ends unexpectedly (expected '%c')."),
			       c);
  else if(*start != c)
    throw CompilationException(_("Expected '%c', got '%c'."),
			       c, *start);

  ++start;
}

template<typename arg>
struct parse_method;

void parse_open_paren(string::const_iterator &start,
		      const string::const_iterator &end)
{
  parse_required_character(start, end, '(');
}

void parse_close_paren(string::const_iterator &start,
		       const string::const_iterator &end)
{
  parse_required_character(start, end, ')');
}

void parse_comma(string::const_iterator &start,
		 const string::const_iterator &end)
{
  parse_required_character(start, end, ',');
}

template<>
struct parse_method<string>
{
  string operator()(string::const_iterator &start,
		    const string::const_iterator &end,
		    const std::vector<const char *> &terminators,
		    bool search_descriptions,
		    bool wide_context) const
  {
    return parse_substr(start, end, std::vector<const char *>(), false);
  }
};

template<>
struct parse_method<pkg_matcher_real *>
{
  pkg_matcher_real *operator()(string::const_iterator &start,
			       const string::const_iterator &end,
			       const std::vector<const char *> &terminators,
			       bool search_descriptions,
			       bool wide_context,
			       const parse_environment &name_context) const
  {
    return parse_condition_list(start, end, terminators, search_descriptions, wide_context, name_context);
  }
};

template<typename T, typename A1>
T *parse_unary_matcher(string::const_iterator &start,
		       const string::const_iterator &end,
		       const std::vector<const char *> &terminators,
		       bool search_descriptions,
		       bool wide_context,
		       const parse_environment &name_context,
		       const parse_method<A1> &parse1 = parse_method<A1>())
{
  parse_open_paren(start, end);
  A1 a1(parse1(start, end, terminators, search_descriptions, wide_context, name_context));
  parse_close_paren(start, end);

  return new T(a1);
}

void add_new_terminator(const char *new_terminator,
			std::vector<const char *> &terminators)
{
  for(std::vector<const char*>::const_iterator it = terminators.begin();
      it != terminators.end(); ++it)
    {
      if(strcmp(new_terminator, *it) == 0)
	return;
    }

  terminators.push_back(new_terminator);
}

template<typename T, typename A1, typename A2>
T *parse_binary_matcher(string::const_iterator &start,
			const string::const_iterator &end,
			const std::vector<const char *> &terminators,
			bool search_descriptions,
			bool wide_context,
			const parse_environment &name_context,
			const parse_method<A1> &parse1 = parse_method<A1>(),
			const parse_method<A2> &parse2 = parse_method<A2>())
{
  std::vector<const char *> terminators_plus_comma(terminators);
  add_new_terminator(",", terminators_plus_comma);

  parse_open_paren(start, end);
  A1 a1(parse1(start, end, terminators_plus_comma, search_descriptions, wide_context, name_context));
  parse_comma(start, end);
  A2 a2(parse2(start, end, terminators, search_descriptions, wide_context, name_context));
  parse_close_paren(start, end);

  return new T(a1, a2);
}

string parse_string_match_args(string::const_iterator &start,
			       const string::const_iterator &end)
{
  parse_open_paren(start, end);
  string substr(parse_substr(start, end, std::vector<const char *>(), false));
  parse_close_paren(start, end);

  return substr;
}

pkg_matcher_real *parse_pkg_matcher_args(string::const_iterator &start,
					 const string::const_iterator &end,
					 const std::vector<const char *> &terminators,
					 bool search_descriptions, bool wide_context,
					 const parse_environment &name_context)
{
  parse_open_paren(start, end);
  auto_ptr<pkg_matcher_real> m(parse_condition_list(start, end, terminators, search_descriptions, wide_context, name_context));
  parse_close_paren(start, end);

  return m.release();
}

pkg_matcher_real *parse_optional_pkg_matcher_args(string::const_iterator &start,
						  const string::const_iterator &end,
						  const std::vector<const char *> terminators,
						  bool search_descriptions,
						  bool wide_context,
						  const parse_environment &name_context)
{
  while(start != end && isspace(*start))
    ++start;

  if(start != end && *start == '(')
    return parse_pkg_matcher_args(start, end, terminators, search_descriptions, wide_context, name_context);
  else
    return NULL;
}

/** \brief Find the index of the given bound variable. */
pkg_matcher_real::match_stack::size_type
get_variable_index(const string &bound_variable,
		   const parse_environment &name_context)
{
  int idx = name_context.get(bound_variable, -1);
  if(idx == -1)
    throw CompilationException("Unknown variable \"%s\".",
			       bound_variable.c_str());
  else
    return idx;
}

/** \brief Parse the tail of a lambda form.
 *
 *  The full lambda form is:
 *
 *    ?for <variable>: CONDITION-LIST
 *
 *  This function assumes that "?for" has been parsed, so it parses:
 *
 *  <variable>: CONDITION-LIST
 */
pkg_matcher_real *parse_explicit_matcher(const std::string &matcher_name,
					 string::const_iterator &start,
					 const string::const_iterator &end,
					 const std::vector<const char *> &terminators,
					 bool search_descriptions,
					 bool wide_context,
					 const parse_environment &name_context)
{
  parse_whitespace(start, end);

  string bound_variable;
  while(start != end && *start != '(' && *start != '!' &&
	*start != '|' && *start != ')' && *start != '?' &&
	*start != '~' && *start != ':' && !isspace(*start) &&
	!terminate(start, end, terminators))
    {
      bound_variable.push_back(*start);
      ++start;
    }

  parse_whitespace(start, end);

  if(start == end)
    throw CompilationException("Unexpected end of pattern following ?%s %s (expected \":\" followed by a search term).",
			       matcher_name.c_str(),
			       bound_variable.c_str());
  else if(*start != ':')
    throw CompilationException("Unexpected '%c' following ?%s %s (expected \":\" followed by a search term).",
			       *start, matcher_name.c_str(), bound_variable.c_str());

  ++start;

  parse_whitespace(start, end);

  // Variables are case-insensitive and normalized to lower-case.
  for(std::string::iterator it = bound_variable.begin();
      it != bound_variable.end(); ++it)
    *it = tolower(*it);

  // Bind the name to the index that the variable will have in the
  // stack (counting from the bottom of the stack to the top).
  parse_environment name_context2(parse_environment::bind(name_context,
							  bound_variable,
							  name_context.size()));
  std::auto_ptr<pkg_matcher_real> m(parse_condition_list(start, end,
							 terminators,
							 search_descriptions,
							 wide_context,
							 name_context2));

  return new pkg_explicit_matcher(m.release());
}

/** \brief Return a matcher that may or may not have a rebound
 *  variable.
 *
 *  If bound_variable is an empty string, just returns matcher.
 *  Otherwise, looks up bound_variable in the local environment
 *  (throwing a CompilationException if the lookup fails) and
 *  generates a pkg_bind_matcher that wraps the given matcher.
 */
pkg_matcher_real *maybe_bind(const string &bound_variable,
			     pkg_matcher_real *matcher,
			     const parse_environment &name_context)
{
  if(bound_variable.empty())
    return matcher;
  else
    return new pkg_bind_matcher(matcher,
				get_variable_index(bound_variable,
						   name_context));
}

pkg_matcher_real *parse_matcher_args(const string &matcher_name,
				     string::const_iterator &start,
				     const string::const_iterator &end,
				     const vector<const char *> &terminators,
				     bool search_descriptions,
				     bool wide_context,
				     const parse_environment &name_context)
{
  {
    // This block parses the following forms:
    //
    // ?TYPE(term)
    // ?broken-TYPE
    // ?broken-TYPE(term)
    // ?reverse-TYPE(term)
    // ?broken-reverse-TYPE(term)
    // ?reverse-broken-TYPE(term)
    const std::string broken_prefix("broken-");
    const std::string reverse_prefix("reverse-");

    bool broken = false;
    bool reverse = false;
    std::string suffix;

    if(std::string(matcher_name, 0, broken_prefix.size()) == broken_prefix)
      {
	broken = true;

	if(std::string(matcher_name, broken_prefix.size(), reverse_prefix.size()) == reverse_prefix)
	  {
	    reverse = true;
	    suffix = std::string(matcher_name, broken_prefix.size() + reverse_prefix.size());
	  }
	else
	  suffix = std::string(matcher_name, broken_prefix.size());
      }
    else if(std::string(matcher_name, 0, reverse_prefix.size()) == reverse_prefix)
      {
	reverse = true;

	if(std::string(matcher_name, reverse_prefix.size(), broken_prefix.size()) == broken_prefix)
	  {
	    broken = true;
	    suffix = std::string(matcher_name, broken_prefix.size() + reverse_prefix.size());
	  }
	else
	  suffix = std::string(matcher_name, reverse_prefix.size());
      }
    else
      suffix = matcher_name;

    const pkgCache::Dep::DepType deptype = parse_deptype(suffix);

    while(start != end && isspace(*start) &&
	  !terminate(start, end, terminators))
      ++start;

    if(deptype == -1)
      {
	// Handle the special case of reverse-provides.
	if(reverse && suffix == "provides")
	  return new pkg_revprv_matcher(parse_pkg_matcher_args(start, end,
							       terminators,
							       search_descriptions,
							       false,
							       name_context));
	else if(broken || reverse)
	  throw CompilationException(_("Unknown dependency type: %s"),
				     suffix.c_str());

	// Otherwise what we have isn't a dep-matcher at all, so just
	// don't do anything and try other options.
      }
    else
      {
	if(reverse)
	  {
	    // broken-reverse-TYPE(term) and reverse-broken-TYPE(term)
	    pkg_matcher_real *m(parse_pkg_matcher_args(start, end,
						       terminators,
						       search_descriptions,
						       false,
						       name_context));

	    return new pkg_revdep_matcher(deptype, m, broken);
	  }
	else
	  {
	    // broken-TYPE and broken-TYPE(term) in the first branch,
	    // TYPE(term) in the second.
	    auto_ptr<pkg_matcher_real> m(broken
					 ? parse_optional_pkg_matcher_args(start, end, terminators, search_descriptions, false, name_context)
					 : parse_pkg_matcher_args(start, end, terminators, search_descriptions, false, name_context));

	    if(m.get() != NULL)
	      return new pkg_dep_matcher(deptype, m.release(), broken);
	    else
	      return new pkg_broken_type_matcher(deptype);
	  }
      }
  }

  matcher_type type;
  bool found = false;

  // Hokey sequential scan.  Why?  Allocating a static map and
  // populating it raises icky issues of thread-safety, when the
  // initializer runs, etc...I'd rather just accept some (hopefully
  // minor) inefficiency.
  for(const matcher_info *it = matcher_types;
      !found && (unsigned)(it - matcher_types) < (sizeof(matcher_types) / sizeof(matcher_types[0]));
      ++it)
    {
      if(matcher_name == it->name)
	{
	  type = it->type;
	  found = true;
	}
    }

  if(!found)
    throw CompilationException(_("Unknown matcher type: \"%s\"."),
			       matcher_name.c_str());

  switch(type)
    {
    case matcher_type_action:
      return make_action_matcher(parse_string_match_args(start, end));
    case matcher_type_all:
      if(!wide_context)
	/* ForTranslators: Question marks ("?") are used as prefix for function names.
	   Leave the question marks attached to the string placeholders. */
	throw CompilationException(_("The ?%s matcher must be used in a \"wide\" context (a top-level context, or a context enclosed by ?%s)."),
				   matcher_name.c_str(),
				   "widen");
      else
	return new pkg_all_matcher(parse_pkg_matcher_args(start, end, terminators, search_descriptions, false, name_context));
    case matcher_type_and:
      return parse_binary_matcher<pkg_and_matcher, pkg_matcher_real *, pkg_matcher_real *>(start, end, terminators, search_descriptions, wide_context, name_context);
    case matcher_type_any:
      if(!wide_context)
	throw CompilationException(_("The ?%s matcher must be used in a \"wide\" context (a top-level context, or a context enclosed by ?%s)."),
				   matcher_name.c_str(),
				   // TODO: should this be the translated
				   // form of ?widen?
				   "widen");
      else
	return new pkg_any_matcher(parse_pkg_matcher_args(start, end, terminators, search_descriptions, false, name_context));
    case matcher_type_archive:
      return new pkg_archive_matcher(parse_string_match_args(start, end));
    case matcher_type_automatic:
      return new pkg_auto_matcher;
    case matcher_type_bind:
      {
	parse_whitespace(start, end);
	parse_open_paren(start, end);

	std::vector<const char *> new_terminators;
	new_terminators.push_back(")");
	new_terminators.push_back(",");
	std::string variable_name = parse_substr(start, end, new_terminators, true);
	int idx = get_variable_index(variable_name, name_context);

	parse_whitespace(start, end);
	parse_comma(start, end);
	parse_whitespace(start, end);

	// Remove the comma we pushed at the end of this list, since
	// it's no longer a terminator.
	new_terminators.pop_back();

	pkg_matcher_real *m = parse_condition_list(start, end, new_terminators, search_descriptions, wide_context, name_context);
	parse_whitespace(start, end);
	parse_close_paren(start, end);

	return new pkg_bind_matcher(m, idx);
      }
    case matcher_type_broken:
      return new pkg_broken_matcher;
    case matcher_type_config_files:
      return new pkg_configfiles_matcher;
    case matcher_type_description:
      return new pkg_description_matcher(parse_string_match_args(start, end));
    case matcher_type_essential:
      return new pkg_essential_matcher;
    case matcher_type_false:
      return new pkg_false_matcher;
    case matcher_type_for:
      return parse_explicit_matcher(matcher_name, start, end, terminators, search_descriptions, wide_context, name_context);
    case matcher_type_garbage:
      return new pkg_garbage_matcher;
    case matcher_type_installed:
      return new pkg_installed_matcher;
    case matcher_type_maintainer:
      return new pkg_maintainer_matcher(parse_string_match_args(start, end));
    case matcher_type_name:
      return new pkg_name_matcher(parse_string_match_args(start, end));
    case matcher_type_narrow:
      return parse_binary_matcher<pkg_select_matcher, pkg_matcher_real *, pkg_matcher_real *>(start, end, terminators, search_descriptions, false, name_context);
    case matcher_type_new:
      return new pkg_new_matcher;
    case matcher_type_not:
      return new pkg_not_matcher(parse_pkg_matcher_args(start, end, terminators, search_descriptions, wide_context, name_context));
    case matcher_type_obsolete:
      return new pkg_obsolete_matcher;
    case matcher_type_or:
      return parse_binary_matcher<pkg_or_matcher, pkg_matcher_real *, pkg_matcher_real *>(start, end, terminators, search_descriptions, wide_context, name_context);
    case matcher_type_origin:
      return new pkg_origin_matcher(parse_string_match_args(start, end));
    case matcher_type_priority:
      return new pkg_priority_matcher(parse_priority(parse_string_match_args(start, end)));
    case matcher_type_provides:
      return parse_unary_matcher<pkg_provides_matcher, pkg_matcher_real *>(start, end, terminators, search_descriptions, false, name_context);
    case matcher_type_section:
      return new pkg_section_matcher(parse_string_match_args(start, end));
    case matcher_type_source_package:
      return new pkg_source_package_matcher(parse_string_match_args(start, end));
    case matcher_type_source_version:
      return new pkg_source_version_matcher(parse_string_match_args(start, end));
    case matcher_type_tag:
      return new pkg_tag_matcher(parse_string_match_args(start, end));
    case matcher_type_task:
      return new pkg_task_matcher(parse_string_match_args(start, end));
    case matcher_type_true:
      return new pkg_true_matcher;
    case matcher_type_upgradable:
      return new pkg_upgradable_matcher;
    case matcher_type_user_tag:
      return new pkg_user_tag_matcher(parse_string_match_args(start, end));
    case matcher_type_version:
      return make_package_version_matcher(parse_string_match_args(start, end));
    case matcher_type_widen:
      return new pkg_widen_matcher(parse_pkg_matcher_args(start, end, terminators, search_descriptions, true, name_context));
    case matcher_type_virtual:
      return new pkg_virtual_matcher;
    default:
      // This represents an internal error: it should never happen and
      // won't be comprehensible to the user, so there's no point in
      // translating it (if it does happen they should report the
      // literal message to me).
      throw CompilationException("Unexpected matcher type %d encountered.",
				 type);
    }
}

pkg_matcher_real *parse_function_style_matcher_tail(string::const_iterator &start,
						    const string::const_iterator &end,
						    const vector<const char *> &terminators,
						    bool search_descriptions,
						    bool wide_context,
						    const parse_environment &name_context)
{
  if(*start == '=')
    {
      ++start;

      parse_whitespace(start, end);

      string bound_variable;
      while(start != end && *start != '(' && *start != '!' &&
	    *start != '|' && *start != ')' && *start != '?' &&
	    *start != '~' && *start != ':' && !isspace(*start) &&
	    !terminate(start, end, terminators))
	{
	  bound_variable.push_back(*start);
	  ++start;
	}


      if(bound_variable.empty())
	throw CompilationException("Unexpected end of pattern following ?=%s (expected a variable name).",
				   bound_variable.c_str());
      else
	return new pkg_equal_matcher(get_variable_index(bound_variable,
							name_context));
    }

  // The name is considered to be the next sequence of non-whitespace
  // characters that are not an open paren.

  while(start != end && isspace(*start))
    ++start;

  string raw_name;
  string lower_case_name;
  string bound_variable;
  while(start != end && *start != '(' && *start != '!' &&
	*start != '|' && *start != ')' && *start != '?' &&
	*start != '~' && !isspace(*start) &&
	!terminate(start, end, terminators))
    {
      if(*start == ':')
	{
	  if(!bound_variable.empty())
	    throw CompilationException("Unexpected ':' following \"?%s:%s\".",
				       bound_variable.c_str(), raw_name.c_str());
	  else
	    {
	      bound_variable = raw_name;
	      for(string::iterator it = bound_variable.begin();
		  it != bound_variable.end(); ++it)
		*it = tolower(*it);
	      raw_name.clear();
	      lower_case_name.clear();
	    }
	}
      else
	{
	  raw_name += *start;
	  lower_case_name += tolower(*start);
	}
      ++start;
    }

  return maybe_bind(bound_variable,
		    parse_matcher_args(lower_case_name,
				       start,
				       end,
				       terminators,
				       search_descriptions,
				       wide_context,
				       name_context),
		    name_context);
}

pkg_matcher_real *parse_atom(string::const_iterator &start,
			     const string::const_iterator &end,
			     const vector<const char *> &terminators,
			     bool search_descriptions,
			     bool wide_context,
			     const parse_environment &name_context)
{
  std::string substr;

  while(start != end && isspace(*start))
    ++start;

  while(start != end && *start != '|' && *start != ')' &&
	!terminate(start, end, terminators))
    {
      if(*start == '!')
	{
	  ++start;
	  return new pkg_not_matcher(parse_atom(start, end, terminators,
						search_descriptions,
						wide_context,
						name_context));
	}
      else if(*start == '(')
	// Recur into the list, losing the extra terminators (they are
	// treated normally until the closing paren)
	{
	  ++start;
	  auto_ptr<pkg_matcher_real> lst(parse_condition_list(start, end,
							      vector<const char *>(),
							      search_descriptions,
							      wide_context,
							      name_context));

	  if(!(start != end && *start == ')'))
	    throw CompilationException(_("Unmatched '('"));
	  else
	    {
	      ++start;
	      return lst.release();
	    }
	}
      else if(*start == '?')
	{
	  ++start;
	  return parse_function_style_matcher_tail(start, end, terminators, search_descriptions,
						   wide_context, name_context);
	}
      else if(*start == '~')
	{
	  ++start;
	  while(start != end && isspace(*start))
	    ++start;

	  if(start == end)
	    {
	      if(!search_descriptions)
		return new pkg_name_matcher("~");
	      else
		{
		  auto_ptr<pkg_matcher_real> name(new pkg_name_matcher("~"));
		  auto_ptr<pkg_matcher_real> desc(new pkg_description_matcher(substr));

		  return new pkg_or_matcher(name.release(),
					    desc.release());
		}
	    }
	  else
	    {
	      const char search_flag = *start;

	      ++start;

	      while(start != end && isspace(*start))
		++start;

	      switch(search_flag)
		// Nested switch statements, mmmm...
		// Ok, there really is a reason here.  For all of the match
		// types that need a string argument, some prefix code (see
		// below) is needed to find the string's end.  But this would
		// be worse than unnecessary for the others.  So I have this
		// double check -- first test for anything that doesn't need
		// the prefix code, then work out which of the other forms
		// we have.
		{
		case 'v':
		  return new pkg_virtual_matcher;
		case 'b':
		  return new pkg_broken_matcher;
		case 'g':
		  return new pkg_garbage_matcher;
		case 'c':
		  return new pkg_configfiles_matcher;
		case 'i':
		  return new pkg_installed_matcher;
		case 'E':
		  return new pkg_essential_matcher;
		case 'M':
		  return new pkg_auto_matcher;
		case 'N':
		  return new pkg_new_matcher;
		case 'U':
		  return new pkg_upgradable_matcher;
		case 'o':
		  return new pkg_obsolete_matcher;
		case 'P':
		case 'C':
		case 'W':
		  {
		    auto_ptr<pkg_matcher_real> m(parse_atom(start,
							    end,
							    terminators,
							    search_descriptions,
							    search_flag == 'W',
							    name_context));

		    switch(search_flag)
		      {
		      case 'C':
			return new pkg_dep_matcher(pkgCache::Dep::Conflicts, m.release(), false);
		      case 'P':
			return new pkg_provides_matcher(m.release());
		      case 'W':
			return new pkg_widen_matcher(m.release());
		      }
		  }
		case 'S':
		  {
		    auto_ptr<pkg_matcher_real> filter(parse_atom(start,
								 end,
								 terminators,
								 search_descriptions,
								 false,
								 name_context));

		    auto_ptr<pkg_matcher_real> pattern(parse_atom(start,
								  end,
								  terminators,
								  search_descriptions,
								  false,
								  name_context));

		    return new pkg_select_matcher(filter.release(), pattern.release());
		  }
		case 'D':
		case 'R':
		  {
		    bool do_provides = false;
		    bool broken = false;
		    pkgCache::Dep::DepType type=pkgCache::Dep::Depends;

		    if(start != end && *start == 'B')
		      {
			broken = true;
			++start;
		      }

		    string::const_iterator nextstart = start;

		    while(nextstart != end && isalpha(*nextstart) &&
			  !terminate(nextstart, end, terminators))
		      ++nextstart;

		    while(nextstart != end && isspace(*nextstart))
		      ++nextstart;

		    if(nextstart != end && *nextstart == ':')
		      {
			string tname(start, nextstart);
			stripws(tname);

			start = nextstart;
			++start;

			if(!strcasecmp(tname.c_str(), "provides"))
			  do_provides=true;
			else
			  {
			    type=parse_deptype(tname.c_str());

			    if(type==-1)
			      throw CompilationException(_("Unknown dependency type: %s"),
							 tname.c_str());
			  }
		      }

		    if(do_provides && broken)
		      throw CompilationException(_("Provides: cannot be broken"));

		    auto_ptr<pkg_matcher_real> m(parse_atom(start, end, terminators,
							    search_descriptions,
							    false, name_context));

		    switch(search_flag)
		      {
		      case 'D':
			if(do_provides)
			  return new pkg_provides_matcher(m.release());
			else
			  return new pkg_dep_matcher(type, m.release(), broken);
		      case 'R':
			if(do_provides)
			  return new pkg_revprv_matcher(m.release());
			else
			  return new pkg_revdep_matcher(type, m.release(), broken);
		      }
		  }
		default:
		  substr = parse_substr(start, end, terminators, true);
		  switch(search_flag)
		    {
		    case 'a':
		      {
			return make_action_matcher(substr.c_str());
		      }
		    case 'A':
		      return new pkg_archive_matcher(substr);
		    case 'B':
		      {
			pkgCache::Dep::DepType ptype=parse_deptype(substr);

			if(ptype!=-1)
			  return new pkg_broken_type_matcher(ptype);
			else
			  throw CompilationException(_("Unknown dependency type: %s"),
						     substr.c_str());
		      }
		    case 'd':
		      return new pkg_description_matcher(substr);
		    case 'G':
		      return new pkg_tag_matcher(substr);
		    case 'F':
		      return new pkg_false_matcher;
		    case 'm':
		      return new pkg_maintainer_matcher(substr);
		    case 'n':
		      return new pkg_name_matcher(substr);
		    case 'O':
		      return new pkg_origin_matcher(substr);
		    case 'p':
		      return new pkg_priority_matcher(parse_priority(substr));
		    case 's':
		      return new pkg_section_matcher(substr);
		    case 't':
		      return new pkg_task_matcher(substr);
		    case 'T':
		      return new pkg_true_matcher;
		    case 'V':
		      return make_package_version_matcher(substr);
		    default:
		      throw CompilationException(_("Unknown pattern type: %c"), search_flag);
		    }
		}
	    }
	}
      else
	{
	  if(!search_descriptions)
	    return new pkg_name_matcher(parse_substr(start, end,
						     terminators, true));
	  else
	    {
	      substr = parse_substr(start, end, terminators, true);
	      auto_ptr<pkg_matcher_real> name(new pkg_name_matcher(substr));
	      auto_ptr<pkg_matcher_real> desc(new pkg_description_matcher(substr));

	      return new pkg_or_matcher(name.release(),
					desc.release());
	    }
	}
    }

  // If we get here, the string was empty.
  throw CompilationException(_("Can't search for \"\""));
}

pkg_matcher_real *parse_and_group(string::const_iterator &start,
				  const string::const_iterator &end,
				  const vector<const char *> &terminators,
				  bool search_descriptions,
				  bool wide_context,
				  const parse_environment &name_context)
{
  auto_ptr<pkg_matcher_real> rval(NULL);
  while(start != end && isspace(*start))
    ++start;

  while(start != end && *start != '|' && *start != ')' &&
	!terminate(start, end, terminators))
    {
      auto_ptr<pkg_matcher_real> atom(parse_atom(start, end, terminators,
						 search_descriptions,
						 wide_context,
						 name_context));

      if(rval.get() == NULL)
	rval = atom;
      else
	rval = auto_ptr<pkg_matcher_real>(new pkg_and_matcher(rval.release(), atom.release()));

      while(start != end && isspace(*start))
	++start;
    }

  if(rval.get() == NULL)
    throw CompilationException(_("Unexpected empty expression"));

  return rval.release();
}

pkg_matcher_real *parse_condition_list(string::const_iterator &start,
				       const string::const_iterator &end,
				       const vector<const char *> &terminators,
				       bool search_descriptions,
				       bool wide_context,
				       const parse_environment &name_context)
{
  auto_ptr<pkg_matcher_real> grp(parse_and_group(start, end, terminators,
						 search_descriptions,
						 wide_context,
						 name_context));

  while(start != end && isspace(*start))
    ++start;

  while(start != end && *start != ')' && !terminate(start, end, terminators))
    {
      if(start != end && *start == '|')
	{
	  ++start;
	  auto_ptr<pkg_matcher_real> grp2(parse_condition_list(start, end, terminators,
							       search_descriptions,
							       wide_context,
							       name_context));

	  return new pkg_or_matcher(grp.release(), grp2.release());
	}
      else
	throw CompilationException(_("Badly formed expression"));

      // Note that this code should never execute:
      while(start != end && isspace(*start))
	++start;
    }

  // If there's no second element in the condition list, return its
  // head.
  return grp.release();
}

}

pkg_matcher *parse_pattern(string::const_iterator &start,
			   const string::const_iterator &end,
			   const std::vector<const char *> &terminators,
			   bool search_descriptions,
			   bool flag_errors,
			   bool require_full_parse)
{
  // Just filter blank strings out immediately.
  while(start != end && isspace(*start) && !terminate(start, end, terminators))
    ++start;

  if(start == end)
    return NULL;

  try
    {
      auto_ptr<pkg_matcher_real> rval(parse_condition_list(start, end, terminators,
							   search_descriptions,
							   true,
							   parse_environment()));

      while(start != end && isspace(*start))
	++start;

      if(require_full_parse && start != end)
	throw CompilationException(_("Unexpected ')'"));
      else
	return rval.release();
    }
  catch(CompilationException e)
    {
      if(flag_errors)
	_error->Error("%s", e.get_msg().c_str());

      return NULL;
    }
}


bool apply_matcher(pkg_matcher *matcher,
		   const pkgCache::PkgIterator &pkg,
		   const pkgCache::VerIterator &ver,
		   aptitudeDepCache &cache,
		   pkgRecords &records)
{
  pkg_matcher_real *real_matcher(dynamic_cast<pkg_matcher_real *>(matcher));
  eassert(real_matcher != NULL);

  pkg_matcher_real::match_stack stack;
  return real_matcher->matches(pkg, ver, cache, records, stack);
}



/** \return a match result, or \b NULL if there is no match.  It's
 *  the caller's responsibility to delete this.
 */
pkg_match_result *get_match(pkg_matcher *matcher,
			    const pkgCache::PkgIterator &pkg,
			    const pkgCache::VerIterator &ver,
			    aptitudeDepCache &cache,
			    pkgRecords &records)
{
  pkg_matcher_real *real_matcher(dynamic_cast<pkg_matcher_real *>(matcher));
  eassert(real_matcher != NULL);

  pkg_matcher_real::match_stack stack;
  return real_matcher->get_match(pkg, ver, cache, records, stack);
}

/** See whether this matches a versionless package.  This applies
 *  the matcher to every version of the package and returns \b true
 *  if any version is matched.
 */
bool apply_matcher(pkg_matcher *matcher,
		   const pkgCache::PkgIterator &pkg,
		   aptitudeDepCache &cache,
		   pkgRecords &records)
{
  pkg_matcher_real *real_matcher(dynamic_cast<pkg_matcher_real *>(matcher));
  eassert(real_matcher != NULL);

  pkg_matcher_real::match_stack stack;
  return real_matcher->matches(pkg, cache, records, stack);
}

/** Get a match result for a versionless package.  This applies the
 *  matcher to each version of the package, returning \b NULL if
 *  none matches or the first match found otherwise.
 */
pkg_match_result *get_match(pkg_matcher *matcher,
			    const pkgCache::PkgIterator &pkg,
			    aptitudeDepCache &cache,
			    pkgRecords &records)
{
  pkg_matcher_real *real_matcher(dynamic_cast<pkg_matcher_real *>(matcher));
  eassert(real_matcher != NULL);

  pkg_matcher_real::match_stack stack;
  return real_matcher->get_match(pkg, cache, records, stack);
}

class pkg_const_matcher : public pkg_matcher_real
{
  pkgCache::PkgIterator match_pkg;

  class const_name_result : public pkg_match_result
  {
    std::string name_group;
  public:
    const_name_result(const std::string &_name_group)
      : name_group(_name_group)
    {
    }

    unsigned int num_groups() { return 1; }
    const std::string &group(unsigned int n) { return name_group; }
  };
public:
  pkg_const_matcher(const pkgCache::PkgIterator &_match_pkg)
    : match_pkg(_match_pkg)
  {
  }

  bool matches(const pkgCache::PkgIterator &pkg,
	       const pkgCache::VerIterator &ver,
	       aptitudeDepCache &cache,
	       pkgRecords &records,
	       match_stack &stack)
  {
    return pkg == match_pkg;
  }

  pkg_match_result *get_match(const pkgCache::PkgIterator &pkg,
			      const pkgCache::VerIterator &ver,
			      aptitudeDepCache &cache,
			      pkgRecords &records,
			      match_stack &stack)
  {
    if(pkg == match_pkg)
      return new const_name_result(pkg.Name());
    else
      return NULL;
  }
};

pkg_matcher *make_const_matcher(const pkgCache::PkgIterator &pkg)
{
  return new pkg_const_matcher(pkg);
}


}
}