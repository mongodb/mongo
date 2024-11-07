import {range} from "jstests/product_limits/libs/util.js";
import {PipelineWorkload} from "jstests/product_limits/libs/workload.js";

class WorkloadTextSearch extends PipelineWorkload {
    pipeline(dataset) {
        return [{$match: {$text: {$search: this.searchString(dataset)}}}, {$unset: "_id"}];
    }

    numbersAsStrings() {
        // Those integers are not present in the dataset
        return range(this.scale()).map((i) => (`${i}`));
    }
}
export class WorkloadTextSearchLongString extends WorkloadTextSearch {
    searchString(dataset) {
        return 'x'.repeat(dataset.scale());
    }
}

export class WorkloadTextSearchManyWords extends WorkloadTextSearch {
    searchString() {
        let words = this.numbersAsStrings();
        words.push('x');
        return words.join(' ');
    }
}

export class WorkloadTextSearchNegation extends WorkloadTextSearch {
    searchString() {
        let words = this.numbersAsStrings().map((s) => (`-${s}`));
        words.push('x');
        return words.join(' ');
    }
}
