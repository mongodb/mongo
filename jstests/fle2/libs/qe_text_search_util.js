
class TextFieldBase {
    constructor(lb, ub, caseSensitive, diacriticSensitive, maxContention) {
        this._lb = NumberInt(lb);
        this._ub = NumberInt(ub);
        this._caseSensitive = caseSensitive;
        this._diacriticSensitive = diacriticSensitive;
        this._cm = NumberLong(maxContention);
    }
    createQueryTypeDescriptor() {
        return {
            "contention": this._cm,
            "strMinQueryLength": this._lb,
            "strMaxQueryLength": this._ub,
            "caseSensitive": this._caseSensitive,
            "diacriticSensitive": this._diacriticSensitive,
        };
    }
}

export class SuffixField extends TextFieldBase {
    createQueryTypeDescriptor() {
        return Object.assign({"queryType": "suffixPreview"}, super.createQueryTypeDescriptor());
    }

    calculateExpectedTagCount(byte_len) {
        assert.gt(this._lb, 0);
        assert.gte(this._ub, this._lb);
        assert.gte(byte_len, 0);

        // See
        // https://github.com/10gen/mongo/blob/master/src/mongo/db/modules/enterprise/docs/fle/fle_string_search.md#strencode-suffix-and-prefix
        // for an explanation of this calculation.
        const padded_len = Math.ceil((byte_len + 5) / 16) * 16 - 5;
        if (this._lb > padded_len) {
            return 1;  // 1 is for just the exact match string
        }
        return (Math.min(this._ub, padded_len) - this._lb + 1) +
            1;  // +1 includes tag for exact match string
    }

    calculateExpectedUniqueTagCount(strs) {
        const affixSet = new Set();
        const uniqueStrs = new Set(strs);
        const paddedStrs = new Set();
        for (const str of strs) {
            for (let affix_len = this._lb; affix_len <= Math.min(this._ub, str.length);
                 affix_len++) {
                affixSet.add(str.slice(-affix_len));
            }
            const padded_len = Math.ceil((str.length + 5) / 16) * 16 - 5;
            if (str.length !== padded_len && str.length < this._ub) {
                // This string needs padding.
                paddedStrs.add(str);
            }
        }
        // Number of unique affixes + number of unique strings ( = number of unique exact match
        // tokens) + number of unique strings that need padding ( = number of unique padding
        // tokens).
        return affixSet.size + uniqueStrs.size + paddedStrs.size;
    }
}

export class PrefixField extends SuffixField {
    createQueryTypeDescriptor() {
        let spec = super.createQueryTypeDescriptor();
        spec.queryType = "prefixPreview";
        return spec;
    }

    calculateExpectedUniqueTagCount(strs) {
        const affixSet = new Set();
        const uniqueStrs = new Set(strs);
        const paddedStrs = new Set();
        for (const str of strs) {
            for (let affix_len = this._lb; affix_len <= Math.min(this._ub, str.length);
                 affix_len++) {
                affixSet.add(str.slice(0, affix_len));
            }
            const padded_len = Math.ceil((str.length + 5) / 16) * 16 - 5;
            if (str.length !== padded_len && str.length < this._ub) {
                // This string needs padding.
                paddedStrs.add(str);
            }
        }
        // Number of unique affixes + number of unique strings ( = number of unique exact match
        // tokens) + number of unique strings that need padding ( = number of unique padding
        // tokens).
        return affixSet.size + uniqueStrs.size + paddedStrs.size;
    }
}

export class SubstringField extends TextFieldBase {
    constructor(mlen, lb, ub, caseSensitive, diacriticSensitive, maxContention) {
        super(lb, ub, caseSensitive, diacriticSensitive, maxContention);
        this._mlen = NumberInt(mlen);
    }

    createQueryTypeDescriptor() {
        return Object.assign({"queryType": "substringPreview", "strMaxLength": this._mlen},
                             super.createQueryTypeDescriptor());
    }

    calculateExpectedTagCount(byte_len) {
        assert.gte(byte_len, 0);
        assert.gt(this._lb, 0);
        assert.gte(this._ub, this._lb);
        assert.gte(this._mlen, this._ub);

        // See
        // https://github.com/10gen/mongo/blob/master/src/mongo/db/modules/enterprise/docs/fle/fle_string_search.md#strencode-substring
        // for an explanation of this calculation.
        const padded_len = Math.ceil((byte_len + 5) / 16) * 16 - 5;
        if (byte_len > this._mlen || this._lb > padded_len) {
            return 1;  // 1 is for just the exact match string
        }
        const hi = Math.min(this._ub, padded_len);
        const range = hi - this._lb + 1;
        const hisum = (hi * (hi + 1)) / 2;              // sum of [1..hi]
        const losum = (this._lb * (this._lb - 1)) / 2;  // sum of [1..lb)
        const maxkgram1 = (this._mlen * range) - (hisum - losum) + range;
        const maxkgram2 = (padded_len * range) - (hisum - losum) + range;
        return Math.min(maxkgram1, maxkgram2) + 1;  // +1 includes tag for exact match string
    }

    calculateExpectedUniqueTagCount(strs) {
        const substringSet = new Set();
        const uniqueStrs = new Set(strs);
        const paddedStrs = new Set();

        for (const str of strs) {
            const strSubstringSet = new Set();
            for (let substr_len = this._lb; substr_len <= Math.min(this._ub, str.length);
                 substr_len++) {
                for (let start = 0; start <= str.length - substr_len; start++) {
                    const sub = str.slice(start, start + substr_len);
                    substringSet.add(sub);
                    strSubstringSet.add(sub);
                }
            }
            // msize = expected tag count - 1 (for exact match string).
            if (strSubstringSet.size != this.calculateExpectedTagCount(str.length) - 1) {
                // For substring indexes, we pad when the number of unique substrings is less than
                // the msize (the max possible number of unique substrings given the padded size).
                paddedStrs.add(str);
            }
        }
        // Number of unique affixes + number of unique strings ( = number of unique exact match
        // tokens) + number of unique strings that need padding ( = number of unique padding
        // tokens).
        return substringSet.size + uniqueStrs.size + paddedStrs.size;
    }
}

export class SuffixAndPrefixField {
    constructor(sfxLb, sfxUb, pfxLb, pfxUb, caseSensitive, diacriticSensitive, maxContention) {
        this._suffixField =
            new SuffixField(sfxLb, sfxUb, caseSensitive, diacriticSensitive, maxContention);
        this._prefixField =
            new PrefixField(pfxLb, pfxUb, caseSensitive, diacriticSensitive, maxContention);
    }
    createQueryTypeDescriptor() {
        return [
            this._suffixField.createQueryTypeDescriptor(),
            this._prefixField.createQueryTypeDescriptor()
        ];
    }
    calculateExpectedTagCount(byte_len) {
        // subtract 1 since the exact match string is doubly counted in the other call
        // to calculateExpectedTagCount.
        return this._suffixField.calculateExpectedTagCount(byte_len) +
            this._prefixField.calculateExpectedTagCount(byte_len) - 1;
    }

    calculateExpectedUniqueTagCount(strs) {
        return this._suffixField.calculateExpectedUniqueTagCount(strs) +
            this._prefixField.calculateExpectedUniqueTagCount(strs) -
            (new Set(strs)).size;  // Remove double count of exact match tokens.
    }
}

export function encStrContainsExpr(field, value) {
    return {$expr: {$encStrContains: {input: `$${field}`, substring: value}}};
}

export function encStrStartsWithExpr(field, value) {
    return {$expr: {$encStrStartsWith: {input: `$${field}`, prefix: value}}};
}

export function encStrEndsWithExpr(field, value) {
    return {$expr: {$encStrEndsWith: {input: `$${field}`, suffix: value}}};
}

export function encStrNormalizedEqExpr(field, value) {
    return {$expr: {$encStrNormalizedEq: {input: `$${field}`, string: value}}};
}
