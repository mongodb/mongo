/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_FastBernoulliTrial_h
#define mozilla_FastBernoulliTrial_h

#include "mozilla/Assertions.h"
#include "mozilla/XorShift128PlusRNG.h"

#include <cmath>
#include <stdint.h>

namespace mozilla {

/**
 * class FastBernoulliTrial: Efficient sampling with uniform probability
 *
 * When gathering statistics about a program's behavior, we may be observing
 * events that occur very frequently (e.g., function calls or memory
 * allocations) and we may be gathering information that is somewhat expensive
 * to produce (e.g., call stacks). Sampling all the events could have a
 * significant impact on the program's performance.
 *
 * Why not just sample every N'th event? This technique is called "systematic
 * sampling"; it's simple and efficient, and it's fine if we imagine a
 * patternless stream of events. But what if we're sampling allocations, and the
 * program happens to have a loop where each iteration does exactly N
 * allocations? You would end up sampling the same allocation every time through
 * the loop; the entire rest of the loop becomes invisible to your measurements!
 * More generally, if each iteration does M allocations, and M and N have any
 * common divisor at all, most allocation sites will never be sampled. If
 * they're both even, say, the odd-numbered allocations disappear from your
 * results.
 *
 * Ideally, we'd like each event to have some probability P of being sampled,
 * independent of its neighbors and of its position in the sequence. This is
 * called "Bernoulli sampling", and it doesn't suffer from any of the problems
 * mentioned above.
 *
 * One disadvantage of Bernoulli sampling is that you can't be sure exactly how
 * many samples you'll get: technically, it's possible that you might sample
 * none of them, or all of them. But if the number of events N is large, these
 * aren't likely outcomes; you can generally expect somewhere around P * N
 * events to be sampled.
 *
 * The other disadvantage of Bernoulli sampling is that you have to generate a
 * random number for every event, which can be slow.
 *
 * [significant pause]
 *
 * BUT NOT WITH THIS CLASS! FastBernoulliTrial lets you do true Bernoulli
 * sampling, while generating a fresh random number only when we do decide to
 * sample an event, not on every trial. When it decides not to sample, a call to
 * |FastBernoulliTrial::trial| is nothing but decrementing a counter and
 * comparing it to zero. So the lower your sampling probability is, the less
 * overhead FastBernoulliTrial imposes.
 *
 * Probabilities of 0 and 1 are handled efficiently. (In neither case need we
 * ever generate a random number at all.)
 *
 * The essential API:
 *
 * - FastBernoulliTrial(double P)
 *   Construct an instance that selects events with probability P.
 *
 * - FastBernoulliTrial::trial()
 *   Return true with probability P. Call this each time an event occurs, to
 *   decide whether to sample it or not.
 *
 * - FastBernoulliTrial::trial(size_t n)
 *   Equivalent to calling trial() |n| times, and returning true if any of those
 *   calls do. However, like trial, this runs in fast constant time.
 *
 *   What is this good for? In some applications, some events are "bigger" than
 *   others. For example, large allocations are more significant than small
 *   allocations. Perhaps we'd like to imagine that we're drawing allocations
 *   from a stream of bytes, and performing a separate Bernoulli trial on every
 *   byte from the stream. We can accomplish this by calling |t.trial(S)| for
 *   the number of bytes S, and sampling the event if that returns true.
 *
 *   Of course, this style of sampling needs to be paired with analysis and
 *   presentation that makes the size of the event apparent, lest trials with
 *   large values for |n| appear to be indistinguishable from those with small
 *   values for |n|.
 */
class FastBernoulliTrial {
  /*
   * This comment should just read, "Generate skip counts with a geometric
   * distribution", and leave everyone to go look that up and see why it's the
   * right thing to do, if they don't know already.
   *
   * BUT IF YOU'RE CURIOUS, COMMENTS ARE FREE...
   *
   * Instead of generating a fresh random number for every trial, we can
   * randomly generate a count of how many times we should return false before
   * the next time we return true. We call this a "skip count". Once we've
   * returned true, we generate a fresh skip count, and begin counting down
   * again.
   *
   * Here's an awesome fact: by exercising a little care in the way we generate
   * skip counts, we can produce results indistinguishable from those we would
   * get "rolling the dice" afresh for every trial.
   *
   * In short, skip counts in Bernoulli trials of probability P obey a geometric
   * distribution. If a random variable X is uniformly distributed from [0..1),
   * then std::floor(std::log(X) / std::log(1-P)) has the appropriate geometric
   * distribution for the skip counts.
   *
   * Why that formula?
   *
   * Suppose we're to return |true| with some probability P, say, 0.3. Spread
   * all possible futures along a line segment of length 1. In portion P of
   * those cases, we'll return true on the next call to |trial|; the skip count
   * is 0. For the remaining portion 1-P of cases, the skip count is 1 or more.
   *
   * skip:                0                         1 or more
   *             |------------------^-----------------------------------------|
   * portion:            0.3                            0.7
   *                      P                             1-P
   *
   * But the "1 or more" section of the line is subdivided the same way: *within
   * that section*, in portion P the second call to |trial()| returns true, and in
   * portion 1-P it returns false a second time; the skip count is two or more.
   * So we return true on the second call in proportion 0.7 * 0.3, and skip at
   * least the first two in proportion 0.7 * 0.7.
   *
   * skip:                0                1              2 or more
   *             |------------------^------------^----------------------------|
   * portion:            0.3           0.7 * 0.3          0.7 * 0.7
   *                      P             (1-P)*P            (1-P)^2
   *
   * We can continue to subdivide:
   *
   * skip >= 0:  |------------------------------------------------- (1-P)^0 --|
   * skip >= 1:  |                  ------------------------------- (1-P)^1 --|
   * skip >= 2:  |                               ------------------ (1-P)^2 --|
   * skip >= 3:  |                                 ^     ---------- (1-P)^3 --|
   * skip >= 4:  |                                 .            --- (1-P)^4 --|
   *                                               .
   *                                               ^X, see below
   *
   * In other words, the likelihood of the next n calls to |trial| returning
   * false is (1-P)^n. The longer a run we require, the more the likelihood
   * drops. Further calls may return false too, but this is the probability
   * we'll skip at least n.
   *
   * This is interesting, because we can pick a point along this line segment
   * and see which skip count's range it falls within; the point X above, for
   * example, is within the ">= 2" range, but not within the ">= 3" range, so it
   * designates a skip count of 2. So if we pick points on the line at random
   * and use the skip counts they fall under, that will be indistinguishable
   * from generating a fresh random number between 0 and 1 for each trial and
   * comparing it to P.
   *
   * So to find the skip count for a point X, we must ask: To what whole power
   * must we raise 1-P such that we include X, but the next power would exclude
   * it? This is exactly std::floor(std::log(X) / std::log(1-P)).
   *
   * Our algorithm is then, simply: When constructed, compute an initial skip
   * count. Return false from |trial| that many times, and then compute a new skip
   * count.
   *
   * For a call to |trial(n)|, if the skip count is greater than n, return false
   * and subtract n from the skip count. If the skip count is less than n,
   * return true and compute a new skip count. Since each trial is independent,
   * it doesn't matter by how much n overshoots the skip count; we can actually
   * compute a new skip count at *any* time without affecting the distribution.
   * This is really beautiful.
   */
 public:
  /**
   * Construct a fast Bernoulli trial generator. Calls to |trial()| return true
   * with probability |aProbability|. Use |aState0| and |aState1| to seed the
   * random number generator; both may not be zero.
   */
  FastBernoulliTrial(double aProbability, uint64_t aState0, uint64_t aState1)
   : mGenerator(aState0, aState1)
  {
    setProbability(aProbability);
  }

  /**
   * Return true with probability |mProbability|. Call this each time an event
   * occurs, to decide whether to sample it or not. The lower |mProbability| is,
   * the faster this function runs.
   */
  bool trial() {
    if (mSkipCount) {
      mSkipCount--;
      return false;
    }

    return chooseSkipCount();
  }

  /**
   * Equivalent to calling trial() |n| times, and returning true if any of those
   * calls do. However, like trial, this runs in fast constant time.
   *
   * What is this good for? In some applications, some events are "bigger" than
   * others. For example, large allocations are more significant than small
   * allocations. Perhaps we'd like to imagine that we're drawing allocations
   * from a stream of bytes, and performing a separate Bernoulli trial on every
   * byte from the stream. We can accomplish this by calling |t.trial(S)| for
   * the number of bytes S, and sampling the event if that returns true.
   *
   * Of course, this style of sampling needs to be paired with analysis and
   * presentation that makes the "size" of the event apparent, lest trials with
   * large values for |n| appear to be indistinguishable from those with small
   * values for |n|, despite being potentially much more likely to be sampled.
   */
  bool trial(size_t aCount) {
    if (mSkipCount > aCount) {
      mSkipCount -= aCount;
      return false;
    }

    return chooseSkipCount();
  }

  void setRandomState(uint64_t aState0, uint64_t aState1) {
    mGenerator.setState(aState0, aState1);
  }

  void setProbability(double aProbability) {
    MOZ_ASSERT(0 <= aProbability && aProbability <= 1);
    mProbability = aProbability;
    if (0 < mProbability && mProbability < 1) {
      /*
       * Let's look carefully at how this calculation plays out in floating-
       * point arithmetic. We'll assume IEEE, but the final C++ code we arrive
       * at would still be fine if our numbers were mathematically perfect. So,
       * while we've considered IEEE's edge cases, we haven't done anything that
       * should be actively bad when using other representations.
       *
       * (In the below, read comparisons as exact mathematical comparisons: when
       * we say something "equals 1", that means it's exactly equal to 1. We
       * treat approximation using intervals with open boundaries: saying a
       * value is in (0,1) doesn't specify how close to 0 or 1 the value gets.
       * When we use closed boundaries like [2**-53, 1], we're careful to ensure
       * the boundary values are actually representable.)
       *
       * - After the comparison above, we know mProbability is in (0,1).
       *
       * - The gaps below 1 are 2**-53, so that interval is (0, 1-2**-53].
       *
       * - Because the floating-point gaps near 1 are wider than those near
       *   zero, there are many small positive doubles ε such that 1-ε rounds to
       *   exactly 1. However, 2**-53 can be represented exactly. So
       *   1-mProbability is in [2**-53, 1].
       *
       * - log(1 - mProbability) is thus in (-37, 0].
       *
       *   That range includes zero, but when we use mInvLogNotProbability, it
       *   would be helpful if we could trust that it's negative. So when log(1
       *   - mProbability) is 0, we'll just set mProbability to 0, so that
       *   mInvLogNotProbability is not used in chooseSkipCount.
       *
       * - How much of the range of mProbability does this cause us to ignore?
       *   The only value for which log returns 0 is exactly 1; the slope of log
       *   at 1 is 1, so for small ε such that 1 - ε != 1, log(1 - ε) is -ε,
       *   never 0. The gaps near one are larger than the gaps near zero, so if
       *   1 - ε wasn't 1, then -ε is representable. So if log(1 - mProbability)
       *   isn't 0, then 1 - mProbability isn't 1, which means that mProbability
       *   is at least 2**-53, as discussed earlier. This is a sampling
       *   likelihood of roughly one in ten trillion, which is unlikely to be
       *   distinguishable from zero in practice.
       *
       *   So by forbidding zero, we've tightened our range to (-37, -2**-53].
       *
       * - Finally, 1 / log(1 - mProbability) is in [-2**53, -1/37). This all
       *   falls readily within the range of an IEEE double.
       *
       * ALL THAT HAVING BEEN SAID: here are the five lines of actual code:
       */
      double logNotProbability = std::log(1 - mProbability);
      if (logNotProbability == 0.0)
        mProbability = 0.0;
      else
        mInvLogNotProbability = 1 / logNotProbability;
    }

    chooseSkipCount();
  }

 private:
  /* The likelihood that any given call to |trial| should return true. */
  double mProbability;

  /*
   * The value of 1/std::log(1 - mProbability), cached for repeated use.
   *
   * If mProbability is exactly 0 or exactly 1, we don't use this value.
   * Otherwise, we guarantee this value is in the range [-2**53, -1/37), i.e.
   * definitely negative, as required by chooseSkipCount. See setProbability for
   * the details.
   */
  double mInvLogNotProbability;

  /* Our random number generator. */
  non_crypto::XorShift128PlusRNG mGenerator;

  /* The number of times |trial| should return false before next returning true. */
  size_t mSkipCount;

  /*
   * Choose the next skip count. This also returns the value that |trial| should
   * return, since we have to check for the extreme values for mProbability
   * anyway, and |trial| should never return true at all when mProbability is 0.
   */
  bool chooseSkipCount() {
    /*
     * If the probability is 1.0, every call to |trial| returns true. Make sure
     * mSkipCount is 0.
     */
    if (mProbability == 1.0) {
      mSkipCount = 0;
      return true;
    }

    /*
     * If the probabilility is zero, |trial| never returns true. Don't bother us
     * for a while.
     */
    if (mProbability == 0.0) {
      mSkipCount = SIZE_MAX;
      return false;
    }

    /*
     * What sorts of values can this call to std::floor produce?
     *
     * Since mGenerator.nextDouble returns a value in [0, 1-2**-53], std::log
     * returns a value in the range [-infinity, -2**-53], all negative. Since
     * mInvLogNotProbability is negative (see its comments), the product is
     * positive and possibly infinite. std::floor returns +infinity unchanged.
     * So the result will always be positive.
     *
     * Converting a double to an integer that is out of range for that integer
     * is undefined behavior, so we must clamp our result to SIZE_MAX, to ensure
     * we get an acceptable value for mSkipCount.
     *
     * The clamp is written carefully. Note that if we had said:
     *
     *    if (skipCount > SIZE_MAX)
     *       skipCount = SIZE_MAX;
     *
     * that leads to undefined behavior 64-bit machines: SIZE_MAX coerced to
     * double is 2^64, not 2^64-1, so this doesn't actually set skipCount to a
     * value that can be safely assigned to mSkipCount.
     *
     * Jakub Oleson cleverly suggested flipping the sense of the comparison: if
     * we require that skipCount < SIZE_MAX, then because of the gaps (2048)
     * between doubles at that magnitude, the highest double less than 2^64 is
     * 2^64 - 2048, which is fine to store in a size_t.
     *
     * (On 32-bit machines, all size_t values can be represented exactly in
     * double, so all is well.)
     */
    double skipCount = std::floor(std::log(mGenerator.nextDouble())
                                  * mInvLogNotProbability);
    if (skipCount < SIZE_MAX)
      mSkipCount = skipCount;
    else
      mSkipCount = SIZE_MAX;

    return true;
  }
};

}  /* namespace mozilla */

#endif /* mozilla_FastBernoulliTrial_h */
