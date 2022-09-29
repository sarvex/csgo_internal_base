#pragma once

template<class ty>
concept floating_point = std::is_floating_point_v<ty>;

template<class ty>
concept enumerator = __is_enum(ty);

template<class ty>
concept string_like = std::_Is_any_of_v<ty, std::string, std::string_view,
    std::wstring, std::wstring_view> && requires(ty t)
{
    t.substr();
};

namespace util {

    template<enumerator en>
    inline constexpr auto to_underlying(en e)
    {
        return static_cast<std::underlying_type_t<en>>(e);
    }

    template<class ty>
    inline constexpr ty force_cast(const auto a)
    {
        union { decltype(a) x; ty y; } u{ a };
        return u.y;
    }

    inline constexpr size_t array_size(const auto& x)
    {
        return sizeof(x) / sizeof(x[0]);
    }

}
