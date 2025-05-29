
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
}

export class PrefixField extends SuffixField {
    createQueryTypeDescriptor() {
        let spec = super.createQueryTypeDescriptor();
        spec.queryType = "prefixPreview";
        return spec;
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
