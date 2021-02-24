#ifndef RANDOM_GENERATOR_H
#define RANDOM_GENERATOR_H

#include <algorithm>
#include <cstddef>
#include <random>
#include <string>

namespace test_harness {

/* Helper class to generate random values. */
class random_generator {
    public:
    /* No copies of the singleton allowed. */
    random_generator(random_generator const &) = delete;
    void operator=(random_generator const &) = delete;

    static random_generator &
    get_instance()
    {
        static random_generator _instance;
        return _instance;
    }

    std::string
    generate_string(std::size_t length)
    {
        std::string random_string;

        for (std::size_t i = 0; i < length; ++i)
            random_string += _characters[_distribution(_generator)];

        return (random_string);
    }

    private:
    random_generator()
    {
        _generator = std::mt19937(std::random_device{}());
        _distribution = std::uniform_int_distribution<>(0, _characters.size() - 1);
    }

    std::mt19937 _generator;
    std::uniform_int_distribution<> _distribution;
    const std::string _characters =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
};
} // namespace test_harness

#endif
