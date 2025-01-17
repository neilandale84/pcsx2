/*
 *	Copyright (C) 2007-2009 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#pragma once

#include "GS.h"
#include "GSCodeBuffer.h"
#include "xbyak/xbyak.h"
#include "xbyak/xbyak_util.h"

#include "Renderers/SW/GSScanlineEnvironment.h"

template<class KEY, class VALUE> class GSFunctionMap
{
protected:
	struct ActivePtr
	{
		uint64 frame, frames;
		uint64 ticks, actual, total;
		VALUE f;
	};

	std::unordered_map<KEY, VALUE> m_map;
	std::unordered_map<KEY, ActivePtr*> m_map_active;

	ActivePtr* m_active;

	virtual VALUE GetDefaultFunction(KEY key) = 0;

public:
	GSFunctionMap()
		: m_active(NULL)
	{
	}

	virtual ~GSFunctionMap()
	{
		for(auto &i : m_map_active) delete i.second;
	}

	VALUE operator [] (KEY key)
	{
		m_active = NULL;

		auto it = m_map_active.find(key);

		if(it != m_map_active.end())
		{
			m_active = it->second;
		}
		else
		{
			auto i = m_map.find(key);

			ActivePtr* p = new ActivePtr();

			memset(p, 0, sizeof(*p));

			p->frame = (uint64)-1;

			p->f = i != m_map.end() ? i->second : GetDefaultFunction(key);

			m_map_active[key] = p;

			m_active = p;
		}

		return m_active->f;
	}

	void UpdateStats(uint64 frame, uint64 ticks, int actual, int total)
	{
		if(m_active)
		{
			if(m_active->frame != frame)
			{
				m_active->frame = frame;
				m_active->frames++;
			}

			m_active->ticks += ticks;
			m_active->actual += actual;
			m_active->total += total;

			ASSERT(m_active->total >= m_active->actual);
		}
	}
};

class GSCodeGenerator : public Xbyak::CodeGenerator
{
protected:
	Xbyak::util::Cpu m_cpu;

public:
	GSCodeGenerator(void* code, size_t maxsize)
		: Xbyak::CodeGenerator(maxsize, code)
	{
	}
};

template<class CG, class KEY, class VALUE>
class GSCodeGeneratorFunctionMap : public GSFunctionMap<KEY, VALUE>
{
	std::string m_name;
	void* m_param;
	std::unordered_map<uint64, VALUE> m_cgmap;
	GSCodeBuffer m_cb;
	size_t m_total_code_size;

	enum {MAX_SIZE = 8192};

public:
	GSCodeGeneratorFunctionMap(const char* name, void* param)
		: m_name(name)
		, m_param(param)
		, m_total_code_size(0)
	{
	}

	~GSCodeGeneratorFunctionMap()
	{
#ifdef _DEBUG
		fprintf(stderr, "%s generated %zu bytes of instruction\n", m_name.c_str(), m_total_code_size);
#endif
	}

	VALUE GetDefaultFunction(KEY key)
	{
		VALUE ret = NULL;

		auto i = m_cgmap.find(key);

		if(i != m_cgmap.end())
		{
			ret = i->second;
		}
		else
		{
			void* code_ptr = m_cb.GetBuffer(MAX_SIZE);

			CG* cg = new CG(m_param, key, code_ptr, MAX_SIZE);
			ASSERT(cg->getSize() < MAX_SIZE);

#if 0
			fprintf(stderr, "%s Location:%p Size:%zu Key:%llx\n", m_name.c_str(), code_ptr, cg->getSize(), (uint64)key);
			GSScanlineSelector sel(key);
			sel.Print();
#endif

			m_total_code_size += cg->getSize();

			m_cb.ReleaseBuffer(cg->getSize());

			ret = (VALUE)cg->getCode();

			m_cgmap[key] = ret;

			delete cg;
		}

		return ret;
	}
};
