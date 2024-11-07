import {range} from "jstests/product_limits/libs/util.js";
import {PipelineWorkload} from "jstests/product_limits/libs/workload.js";

export class WorkloadAndOverSingleField extends PipelineWorkload {
    scale() {
        // SERVER-96119 SBE: Stack overflow with many conditions to a $match, index
        return Math.min(1000, super.scale());
    }

    pipeline() {
        let match = [];

        for (let i = 0; i < this.scale(); i++) {
            match.push({'f0': {$lt: this.scale() + i}});
        }

        return [{$match: {$and: match}}, {$unset: "_id"}];
    }

    result() {
        return range(this.scale()).map((i) => ({f0: i}));
    }
}

export class WorkloadOrOverSingleField extends PipelineWorkload {
    scale() {
        // SERVER-96119 SBE: Stack overflow with many conditions to a $match, index
        return Math.min(1000, super.scale());
    }

    pipeline(dataset) {
        let match = [];

        for (let i = 0; i < this.scale(); i++) {
            // Those conditions all evaluate to False
            match.push({'f0': {$gt: this.scale() + i}});
        }

        // This condition evaluates to True
        match.push({'f0': {$lt: dataset.scale()}});

        return [{$match: {$or: match}}, {$unset: "_id"}];
    }
}

export class WorkloadAndPlusOrOverSingleField extends PipelineWorkload {
    pipeline() {
        let match = [];

        for (let i = 0; i < this.scale(); i++) {
            match.push({$or: [{f0: {$lt: this.scale() - i}}, {f0: {$gte: 0}}]});
        }

        return [{$match: {$and: match}}, {$unset: "_id"}];
    }
}

export class WorkloadOrPlusAndOverSingleField extends PipelineWorkload {
    pipeline() {
        let match = [];

        for (let i = 0; i < this.scale(); i++) {
            // These conditions all evaluate to False
            match.push({$and: [{f0: {$gt: this.scale()}}, {f0: {$gte: 0}}]});
        }

        // This condition evaluates to True
        match.push({$and: [{f0: {$lt: this.scale()}}, {f0: {$gte: 0}}]});

        return [{$match: {$or: match}}, {$unset: "_id"}];
    }
}

export class WorkloadAndOverManyFields extends PipelineWorkload {
    pipeline() {
        let match = [];

        for (let i = 0; i < this.scale(); i++) {
            match.push({[`f${i}`]: {$lt: 65535}});
        }

        return [{$match: {$and: match}}, {$unset: "_id"}];
    }
}

export class WorkloadOrOverManyFields extends PipelineWorkload {
    pipeline() {
        let match = [];

        for (let i = 0; i < this.scale(); i++) {
            // All those conditions evaluate to False
            match.push({[`f${i}`]: {$gt: 65535}});
        }

        // This condition evaluates to True
        match.push({[`f${this.scale() - 1}`]: {$gt: 0}});

        return [{$match: {$or: match}}, {$unset: "_id"}];
    }
}

export class WorkloadAndPlusOrOverManyFields extends PipelineWorkload {
    pipeline() {
        let match = [];

        for (let i = 0; i < this.scale(); i++) {
            match.push({$or: [{[`f${i}`]: {$lt: this.scale()}}, {[`f${i}`]: {$gte: 0}}]});
        }

        return [{$match: {$and: match}}, {$unset: "_id"}];
    }
}

export class WorkloadOrPlusAndOverManyFields extends PipelineWorkload {
    pipeline() {
        let match = [];

        for (let i = 0; i < this.scale(); i++) {
            // All those conditions evaluate to False
            match.push({$and: [{[`f${i}`]: {$gt: this.scale()}}, {[`f${i}`]: {$gte: 0}}]});
        }

        // This condition evaluates to True
        match.push({
            $and: [
                {[`f${this.scale() - 1}`]: {$lt: this.scale()}},
                {[`f${this.scale() - 1}`]: {$gte: 0}}
            ]
        });

        return [{$match: {$or: match}}, {$unset: "_id"}];
    }
}

export class WorkloadMatchOverManyFields extends PipelineWorkload {
    /** $match with individual equality conditions over many fields. */
    pipeline() {
        let match = {};

        for (let i = 0; i < this.scale(); i++) {
            match[`f${i}`] = i;
        }

        return [{$match: match}, {$unset: "_id"}];
    }
}
export class WorkloadMatchLongPath extends PipelineWorkload {
    scale() {
        return 100;
    }
    path() {
        let path = [];

        for (let i = 0; i < this.scale(); i++) {
            path.push(`f${i}`);
        }

        return path.join('.');
    }

    pipeline() {
        return [{$match: {[this.path()]: "abc"}}, {$unset: "_id"}, {$count: "cnt"}];
    }

    result() {
        return [{"cnt": 1}];
    }
}

export class WorkloadIn extends PipelineWorkload {
    /** $in */
    pipeline() {
        return [{$match: {f0: {$in: range(this.scale())}}}, {$unset: "_id"}];
    }
}

export class WorkloadNin extends PipelineWorkload {
    /** $nin */
    pipeline() {
        let ninList = [];

        for (let i = 0; i < this.scale(); i++) {
            ninList.push(this.scale() + i);
        }

        return [{$match: {f0: {$nin: ninList}}}, {$unset: "_id"}];
    }
}
export class WorkloadManyIns extends PipelineWorkload {
    /** Many individual $in */
    pipeline() {
        let inList = [];

        for (let i = 0; i < this.scale(); i++) {
            inList.push({f0: {$in: [i]}});
        }

        return [{$match: {$or: inList}}, {$unset: "_id"}];
    }
}

export class WorkloadRegexInIn extends PipelineWorkload {
    /** Multiple regexps in an $in */
    pipeline() {
        let inList = [];

        for (let i = 0; i < this.scale(); i++) {
            inList.push(new RegExp(String.raw`${i}|.*`));
        }

        return [{$match: {f0: {$in: inList}}}, {$unset: "_id"}];
    }
}

export class WorkloadRegex extends PipelineWorkload {
    scale() {
        // Regular expression is invalid: pattern string is longer than the limit set by the
        // application
        return 1000;
    }
    pipeline() {
        let regexStr = new RegExp(range(this.scale()).join("|"));
        return [{$match: {f0: {$regex: regexStr}}}, {$unset: "_id"}];
    }
}

export class WorkloadExists extends PipelineWorkload {
    pipeline() {
        let existsList = {};

        for (let i = 0; i < this.scale(); i++) {
            existsList[`f${i}`] = {$exists: true};
        }
        return [{$match: existsList}, {$count: "cnt"}];
    }

    result() {
        return [{"cnt": 1}];
    }
}

export class WorkloadLongValue extends PipelineWorkload {
    pipeline(dataset) {
        return [{$match: {f0: 'x'.repeat(dataset.scale()) + ' x'}}, {$unset: "_id"}];
    }
}
