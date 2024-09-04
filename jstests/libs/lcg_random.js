/**
 * An implementation of the Lehmer / Parker-Miller linear congruential generator, a fast
 * pseudorandom number generator that can be seeded and of which multiple instances can be created.
 * (JavaScript's native random number generator cannot be seeded and is shared among everything
 * generating random numbers.)
 */
export class LcgRandom {
    // Constructs an instance with a given seed. Any integer can be input as the seed, however for
    // correct operation the seed used to initialize the state must be in the range [1, 2147483646],
    // so if the input seed is outside that range it is coerced into it.
    constructor(seed) {
        seed = seed % 2147483647;
        if (seed < 0) {
            seed *= -1;
        } else if (seed == 0) {
            seed = 1;
        }
        this._seed = seed;
    }

    // Returns an integer value in the range [min, max).
    getRandomInt(min, max) {
        return Math.floor(this.getRandomFloat() * (max - min)) + min;
    }

    // Returns a floating point value in the range [0, 1).
    getRandomFloat() {
        this._seed = this._seed * 48271 % 2147483647;
        return (this._seed - 1) / 2147483646;
    }
}  // class LcgRandom
