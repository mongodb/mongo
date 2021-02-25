import { initialState } from "./store";

export const loading = (state = initialState, action) => {
  switch (action.type) {
    case "setLoading":
      return action.payload;

    default:
      return state;
  }
};

export const setLoading = (loading) => ({
  type: "setLoading",
  payload: loading,
});
