/* Include guard. */
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
    static random_generator *
    get_instance()
    {
        if (!_instance)
            _instance = new random_generator;
        return (_instance);
    }

    std::string
    generate_string(std::size_t length)
    {
        std::string random_string;

        if (length == 0)
            throw std::invalid_argument("random_generator.generate_string: 0 is an invalid length");

        for (std::size_t i = 0; i < length; ++i)
            random_string += _characters[_distribution(_generator)];

        return (random_string);
    }

    private:
    random_generator()
    {
        _generator = std::mt19937(_random_device());
        _distribution = std::uniform_int_distribution<>(0, _characters.size() - 1);
    }

    static random_generator *_instance;
    std::mt19937 _generator;
    std::random_device _random_device;
    std::uniform_int_distribution<> _distribution;
    const std::string _characters =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
};
random_generator *random_generator::_instance = 0;
} // namespace test_harness

#endif
