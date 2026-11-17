#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

namespace Morpthis
{
	namespace Shell
	{
		// Process-wide cache for menu item bitmaps (file/exe icons, rasterized SVGs).
		//
		// Without it every right-click re-extracts icons from disk (ExtractIconEx,
		// SHGetFileInfo, WIC decode) and re-rasterizes every SVG, for every item.
		//
		// Ownership: a bitmap returned with `shared == true` belongs to the cache.
		// Callers store it with IMAGE::inherited = true so IMAGE::destroy() doesn't
		// delete it. Menu drawing only selects a bitmap into a memory DC for the
		// duration of one blit, so a single bitmap can back many items.
		//
		// The cache is cleared when the configuration is unloaded (Initializer::uninit),
		// which is the same point where the parsed config those menus use is freed.
		class BitmapCache
		{
			struct State
			{
				std::mutex mutex;
				std::unordered_map<std::wstring, HBITMAP> map;
			};

			// Intentionally never destroyed: Initializer::uninit() can run from a global
			// destructor at DLL unload, after other statics have been torn down.
			static State &state()
			{
				static State *s = new State;
				return *s;
			}

			// Upper bound on cached GDI handles; past it, bitmaps are returned
			// uncached (owned by the caller) as before.
			static constexpr size_t MaxEntries = 512;

		public:
			static std::wstring key(wchar_t kind, long size, int id, const wchar_t *source)
			{
				std::wstring k;
				k.reserve(32 + (source ? ::wcslen(source) : 0));
				k += kind;
				k += L'|';
				k += std::to_wstring(size);
				k += L'|';
				k += std::to_wstring(id);
				k += L'|';
				if(source) k += source;
				return k;
			}

			// Returns the cached bitmap for `k`, or calls `load()` and caches its result.
			// `shared` tells the caller whether the cache owns the returned bitmap.
			template<typename Loader>
			static HBITMAP get(const std::wstring &k, Loader &&load, bool &shared)
			{
				shared = false;
				auto &st = state();
				{
					std::lock_guard<std::mutex> lock(st.mutex);
					if(auto it = st.map.find(k); it != st.map.end())
					{
						shared = true;
						return it->second;
					}
				}

				// load outside the lock: icon extraction can be slow
				HBITMAP hbitmap = load();
				if(!hbitmap)
					return nullptr;

				std::lock_guard<std::mutex> lock(st.mutex);
				if(auto it = st.map.find(k); it != st.map.end())
				{
					// another thread loaded it meanwhile
					::DeleteObject(hbitmap);
					shared = true;
					return it->second;
				}

				if(st.map.size() >= MaxEntries)
					return hbitmap; // caller owns it

				st.map.emplace(k, hbitmap);
				shared = true;
				return hbitmap;
			}

			static void clear()
			{
				auto &st = state();
				std::lock_guard<std::mutex> lock(st.mutex);
				for(auto &it : st.map)
					::DeleteObject(it.second);
				st.map.clear();
			}
		};
	}
}
