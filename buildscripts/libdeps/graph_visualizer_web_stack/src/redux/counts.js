import { initialState } from "./store";

export const counts = (state = initialState, action) => {
  switch (action.type) {
    case "setCounts":
      return action.payload;

    default:
      return state;
  }
};

export const setCounts = (counts) => ({
  type: "setCounts",
  payload: counts,
});
