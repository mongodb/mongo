package db

type Version [3]int

func (v1 Version) Cmp(v2 Version) int {
	for i := range v1 {
		if v1[i] < v2[i] {
			return -1
		}
		if v1[i] > v2[i] {
			return 1
		}
	}
	return 0
}

func (v1 Version) LT(v2 Version) bool {
	return v1.Cmp(v2) == -1
}

func (v1 Version) LTE(v2 Version) bool {
	return v1.Cmp(v2) != 1
}

func (v1 Version) GT(v2 Version) bool {
	return v1.Cmp(v2) == 1
}

func (v1 Version) GTE(v2 Version) bool {
	return v1.Cmp(v2) != -1
}
